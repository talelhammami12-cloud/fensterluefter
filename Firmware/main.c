/**
 * @file    main.c
 * @brief   Fensterluefter - Firmware fuer das FLY-Super8 Pro (STM32H723ZGT6)
 *
 * Was das Programm macht:
 *   Es schaltet alle 10 Luefter-Anschluesse FAN0 ... FAN9 dauerhaft ein.
 *   Jeder Luefter steckt mit seinem eigenen Stecker in einem Anschluss,
 *   nichts muss abgeschnitten oder geloetet werden.
 *   Alle Luefter laufen mit Vollgas und blasen frische Luft ins Zimmer.
 *
 * Wie das Einschalten funktioniert:
 *   Hinter jedem FAN-Anschluss sitzt ein Transistor (MOSFET). Ist der Pin
 *   des Mikrocontrollers HIGH, schaltet der Transistor durch und der
 *   Luefter bekommt Strom. Wir muessen also nur 10 Pins auf HIGH setzen.
 *
 *   Anschluss | Pin       Anschluss | Pin
 *   FAN0      | PA0       FAN5      | PB11
 *   FAN1      | PA1       FAN6      | PB10
 *   FAN2      | PA2       FAN7      | PD12
 *   FAN3      | PA3       FAN8      | PD14
 *   FAN4      | PA15      FAN9      | PD15
 *
 * Kein HAL, kein CMSIS-Header noetig: Die paar Register stehen unten mit
 * ihrer Adresse aus dem Reference Manual RM0468.
 */

#include <stdint.h>

/** Macht aus einer Adresse ein Register, das man lesen/schreiben kann. */
#define REG(addr)       (*(volatile uint32_t *)(addr))

/* RCC: schaltet den Takt fuer die GPIO-Ports ein */
#define RCC_AHB4ENR     REG(0x58024400u + 0x0E0u)  /* Bit 0=A, 1=B, 3=D */

/* Anfangsadressen der drei GPIO-Ports, die wir brauchen */
#define GPIOA_BASE      0x58020000u
#define GPIOB_BASE      0x58020400u
#define GPIOD_BASE      0x58020C00u

/* Register innerhalb eines Ports */
#define GPIO_MODER(base)  REG((base) + 0x00u)  /* Modus: Eingang/Ausgang */
#define GPIO_BSRR(base)   REG((base) + 0x18u)  /* Pin auf HIGH/LOW setzen */

/* PWR: Spannungsversorgung des Chips (RM0468 Kap. 6) */
#define PWR_CSR1        REG(0x58024800u + 0x04u)   /* Bit 13 = ACTVOSRDY */
#define PWR_CR3         REG(0x58024800u + 0x0Cu)   /* Bit 1 = LDOEN      */

/**
 * Stellt die Versorgung auf den internen Spannungsregler (LDO) ein.
 * Beim STM32H7 muss das nach jedem Reset einmal gemacht werden, sonst
 * bleibt der Chip in einem Wartezustand ("Run*"-Modus).
 */
static void powerInit(void)
{
    /* Nur LDO an: SCUEN (Bit 2), LDOEN (Bit 1), BYPASS (Bit 0) */
    PWR_CR3 = (PWR_CR3 & ~0x7u) | (1u << 1);

    /* Warten bis die Spannung bereit ist (mit Zeitgrenze) */
    for (uint32_t t = 0u; t < 1000000u; t++)
    {
        if ((PWR_CSR1 & (1u << 13)) != 0u)
        {
            break;
        }
    }
}

/**
 * Schaltet einen Luefter-Anschluss ein.
 * Zuerst wird der Pin auf HIGH gesetzt, danach auf "Ausgang" gestellt.
 * So ist der Pin sofort HIGH, sobald er Ausgang wird.
 */
static void fanOn(uint32_t base, uint32_t pin)
{
    /* Bit "pin" in BSRR setzt den Pin auf HIGH */
    GPIO_BSRR(base) = (1u << pin);

    /* Modus-Bits fuer diesen Pin: 01 = normaler Ausgang */
    GPIO_MODER(base) &= ~(3u << (2u * pin));
    GPIO_MODER(base) |=  (1u << (2u * pin));
}

int main(void)
{
    powerInit();

    /* Takt fuer Port A, B und D einschalten, danach einmal lesen */
    RCC_AHB4ENR |= (1u << 0) | (1u << 1) | (1u << 3);
    (void)RCC_AHB4ENR;

    /* Linkes Luefterteil */
    fanOn(GPIOA_BASE, 0u);    /* FAN0 */
    fanOn(GPIOA_BASE, 1u);    /* FAN1 */
    fanOn(GPIOA_BASE, 2u);    /* FAN2 */
    fanOn(GPIOA_BASE, 3u);    /* FAN3 */
    fanOn(GPIOA_BASE, 15u);   /* FAN4 */

    /* Rechtes Luefterteil */
    fanOn(GPIOB_BASE, 11u);   /* FAN5 */
    fanOn(GPIOB_BASE, 10u);   /* FAN6 */
    fanOn(GPIOD_BASE, 12u);   /* FAN7 */
    fanOn(GPIOD_BASE, 14u);   /* FAN8 */
    fanOn(GPIOD_BASE, 15u);   /* FAN9 */

    /* Fuer immer: alle 10 Pins immer wieder auf HIGH setzen */
    for (;;)
    {
        GPIO_BSRR(GPIOA_BASE) = (1u << 0) | (1u << 1) | (1u << 2) |
                                (1u << 3) | (1u << 15);
        GPIO_BSRR(GPIOB_BASE) = (1u << 10) | (1u << 11);
        GPIO_BSRR(GPIOD_BASE) = (1u << 12) | (1u << 14) | (1u << 15);
    }
}
