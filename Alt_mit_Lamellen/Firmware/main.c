/**
 * @file    main.c
 * @brief   Fensterluefter - Firmware fuer das FLY-Super8 Pro (STM32H723ZGT6)
 *
 * Was das Programm macht:
 *   Es laesst zwei Luefter-Gruppen mit fester Geschwindigkeit laufen.
 *     FAN0  (Pin PA0, Timer TIM2 Kanal 1)  ->  Zuluft  (5 Luefter)
 *     FAN1  (Pin PA1, Timer TIM2 Kanal 2)  ->  Abluft  (6 Luefter)
 *   Beim Einschalten laufen beide Gruppen kurz mit 100 %, damit jeder
 *   Luefter sicher anlaeuft. Danach schaltet es auf die eingestellten Werte.
 *
 * Takt:
 *   Wir benutzen den internen HSI-Oszillator mit 64 MHz. Das ist der
 *   Standard nach dem Reset - deshalb muss hier nichts am Takt eingestellt
 *   werden. Timer TIM2 bekommt ebenfalls 64 MHz.
 *
 * Kein HAL, kein CMSIS-Header noetig: Die paar Register, die wir brauchen,
 * stehen unten mit ihrer Adresse aus dem Reference Manual RM0468.
 */

#include <stdint.h>

/* ===================== Einstellungen - nur hier aendern ================== */

/** Geschwindigkeit der Zuluft in Prozent (0 ... 100). */
#define SUPPLY_PERCENT   70u

/** Geschwindigkeit der Abluft in Prozent (0 ... 100). */
#define EXHAUST_PERCENT  60u

/**
 * PWM-Frequenz in Hertz.
 * 25000 = 25 kHz, unhoerbar. Falls die Luefter bei kleinen Prozentwerten
 * kaum langsamer werden, probiere 100 (dann evtl. leises Ticken).
 */
#define PWM_FREQ_HZ      25000u

/** Wie lange beim Start mit 100 % angelaufen wird (Millisekunden). */
#define KICK_MS          2000u

/* ===================== Register (RM0468) ================================= */

/** Macht aus einer Adresse ein Register, das man lesen/schreiben kann. */
#define REG(addr)       (*(volatile uint32_t *)(addr))

/* RCC: schaltet den Takt fuer GPIOA und TIM2 ein */
#define RCC_BASE        0x58024400u
#define RCC_AHB4ENR     REG(RCC_BASE + 0x0E0u)   /* Bit 0  = GPIOAEN */
#define RCC_APB1LENR    REG(RCC_BASE + 0x0E8u)   /* Bit 0  = TIM2EN  */

/* GPIOA: Pins PA0 und PA1 */
#define GPIOA_BASE      0x58020000u
#define GPIOA_MODER     REG(GPIOA_BASE + 0x00u)
#define GPIOA_PUPDR     REG(GPIOA_BASE + 0x0Cu)
#define GPIOA_AFRL      REG(GPIOA_BASE + 0x20u)

/* TIM2: 32-Bit-Timer, erzeugt die PWM */
#define TIM2_BASE       0x40000000u
#define TIM2_CR1        REG(TIM2_BASE + 0x00u)
#define TIM2_EGR        REG(TIM2_BASE + 0x14u)
#define TIM2_CCMR1      REG(TIM2_BASE + 0x18u)
#define TIM2_CCER       REG(TIM2_BASE + 0x20u)
#define TIM2_PSC        REG(TIM2_BASE + 0x28u)
#define TIM2_ARR        REG(TIM2_BASE + 0x2Cu)
#define TIM2_CCR1       REG(TIM2_BASE + 0x34u)
#define TIM2_CCR2       REG(TIM2_BASE + 0x38u)

/* SysTick: fuer die Warte-Funktion */
#define SYST_CSR        REG(0xE000E010u)
#define SYST_RVR        REG(0xE000E014u)
#define SYST_CVR        REG(0xE000E018u)

/** Takt von TIM2 und CPU nach dem Reset (HSI). */
#define CLOCK_HZ        64000000u

/** Zaehlerschritte pro PWM-Periode (64 MHz / 25 kHz = 2560). */
#define PWM_PERIOD      (CLOCK_HZ / PWM_FREQ_HZ)

/* ===================== Hilfsfunktionen =================================== */

/**
 * Rechnet Prozent in einen Vergleichswert fuer den Timer um.
 * Beispiel: 70 % von 2560 Schritten = 1792.
 */
static uint32_t percentToCompare(uint32_t percent)
{
    if (percent > 100u)
    {
        percent = 100u;
    }
    return (PWM_PERIOD * percent) / 100u;
}

/**
 * Wartet die angegebene Zeit. SysTick zaehlt jede Millisekunde einmal
 * herunter, wir warten einfach so oft auf das "fertig"-Bit.
 */
static void delayMs(uint32_t ms)
{
    /* 64 000 Takte = 1 ms, Taktquelle = CPU-Takt, SysTick an */
    SYST_RVR = (CLOCK_HZ / 1000u) - 1u;
    SYST_CVR = 0u;
    SYST_CSR = (1u << 2) | (1u << 0);

    while (ms > 0u)
    {
        /* Bit 16 (COUNTFLAG) wird 1, wenn eine Millisekunde um ist */
        if ((SYST_CSR & (1u << 16)) != 0u)
        {
            ms--;
        }
    }
    SYST_CSR = 0u;
}

/** Stellt PA0 und PA1 auf "Alternate Function 1" (= TIM2). */
static void initPins(void)
{
    /* Takt fuer GPIOA einschalten, danach einmal lesen (kurz warten) */
    RCC_AHB4ENR |= (1u << 0);
    (void)RCC_AHB4ENR;

    /* PA0 und PA1: Modus 10 = Alternate Function */
    GPIOA_MODER &= ~((3u << 0) | (3u << 2));
    GPIOA_MODER |=  ((2u << 0) | (2u << 2));

    /* Kein Pull-up / Pull-down */
    GPIOA_PUPDR &= ~((3u << 0) | (3u << 2));

    /* AF1 fuer PA0 (Bits 3..0) und PA1 (Bits 7..4) */
    GPIOA_AFRL &= ~((0xFu << 0) | (0xFu << 4));
    GPIOA_AFRL |=  ((1u << 0) | (1u << 4));
}

/** Startet TIM2 mit PWM auf Kanal 1 (Zuluft) und Kanal 2 (Abluft). */
static void initPwm(void)
{
    /* Takt fuer TIM2 einschalten */
    RCC_APB1LENR |= (1u << 0);
    (void)RCC_APB1LENR;

    TIM2_PSC = 0u;                    /* kein Vorteiler: 64 MHz */
    TIM2_ARR = PWM_PERIOD - 1u;       /* eine Periode = 2560 Schritte */

    /* Kanal 1 und 2: PWM-Modus 1 (110) + Preload an */
    TIM2_CCMR1 = (6u << 4) | (1u << 3) | (6u << 12) | (1u << 11);

    /* Beide Ausgaenge einschalten (CC1E, CC2E) */
    TIM2_CCER = (1u << 0) | (1u << 4);

    TIM2_CCR1 = 0u;
    TIM2_CCR2 = 0u;

    TIM2_CR1 = (1u << 7);             /* ARR-Preload an */
    TIM2_EGR = (1u << 0);             /* Werte uebernehmen */
    TIM2_CR1 |= (1u << 0);            /* Timer starten */
}

/** Setzt beide Luefter-Gruppen auf die gewuenschten Prozent. */
static void setFans(uint32_t supplyPercent, uint32_t exhaustPercent)
{
    TIM2_CCR1 = percentToCompare(supplyPercent);
    TIM2_CCR2 = percentToCompare(exhaustPercent);
}

/* ===================== Hauptprogramm ===================================== */

int main(void)
{
    initPins();
    initPwm();

    /* Anlauf: kurz Vollgas, damit kein Luefter haengen bleibt */
    setFans(100u, 100u);
    delayMs(KICK_MS);

    /* Danach die eingestellten Werte */
    setFans(SUPPLY_PERCENT, EXHAUST_PERCENT);

    /* Der Timer macht die PWM alleine - die CPU muss nichts mehr tun */
    for (;;)
    {
    }
}
