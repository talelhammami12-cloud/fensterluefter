/**
 * @file    main.c
 * @brief   Lueftungssystem - steuerbare Firmware fuer das FLY-Super8 Pro (STM32H723)
 *
 * Was das Programm macht:
 *   - Regelt die Drehzahl aller 10 Luefter-Anschluesse FAN0...FAN9 mit PWM
 *     (Hardware-Timer TIM2, TIM4, TIM5 - der Strom wird schnell an/aus
 *     geschaltet, das Verhaeltnis an/aus = Geschwindigkeit).
 *   - Meldet sich per USB-Kabel als serielle Schnittstelle (COM-Port).
 *     Die App "Lueftungssystem" schickt Textbefehle, z.B. "fan 3 duty 60".
 *   - Effekte laufen direkt auf dem Board (auch ohne PC):
 *     Manuell, Welle, Brise, Atmen, Lauflicht.
 *   - Schlaf-Timer, Boost, Sanftanlauf (Rampe), Anlauf-Kick, Mindestdrehzahl.
 *   - "save" speichert alles im Flash -> nach Stromausfall wieder da.
 *
 *   Anschluss | Pin  | Timer-Kanal     Anschluss | Pin  | Timer-Kanal
 *   FAN0      | PA0  | TIM5 CH1        FAN5      | PB11 | TIM2 CH4
 *   FAN1      | PA1  | TIM5 CH2        FAN6      | PB10 | TIM2 CH3
 *   FAN2      | PA2  | TIM5 CH3        FAN7      | PD12 | TIM4 CH1
 *   FAN3      | PA3  | TIM5 CH4        FAN8      | PD14 | TIM4 CH3
 *   FAN4      | PA15 | TIM2 CH1        FAN9      | PD15 | TIM4 CH4
 */

#include "stm32h7xx.h"
#include "tusb.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define FW_VERSION   "1.0"
#define NFAN         10u
#define TIM_CLK      64000000u              /* Timer-Takt nach dem Reset (HSI) */
#define SET_ADDR     0x080E0000u            /* Flash-Sektor 7 fuer Einstellungen */
#define SET_MAGIC    0x4C534631u            /* "LSF1" */

/* ------------------------------------------------------------------ */
/* Hardware-Tabelle: welcher Pin / Timer-Kanal gehoert zu welchem FAN  */
/* ------------------------------------------------------------------ */
typedef struct
{
    GPIO_TypeDef *port;
    uint8_t pin;
    uint8_t af;          /* Alternativ-Funktion: Pin wird vom Timer gesteuert */
    TIM_TypeDef *tim;
    uint8_t ch;          /* Timer-Kanal 1...4 */
} FanHw;

static const FanHw HW[NFAN] =
{
    {GPIOA, 0, 2, TIM5, 1}, {GPIOA, 1, 2, TIM5, 2}, {GPIOA, 2, 2, TIM5, 3},
    {GPIOA, 3, 2, TIM5, 4}, {GPIOA, 15, 1, TIM2, 1}, {GPIOB, 11, 1, TIM2, 4},
    {GPIOB, 10, 1, TIM2, 3}, {GPIOD, 12, 2, TIM4, 1}, {GPIOD, 14, 2, TIM4, 3},
    {GPIOD, 15, 2, TIM4, 4}
};

/* ------------------------------------------------------------------ */
/* Einstellungen (werden mit "save" im Flash gespeichert)              */
/* ------------------------------------------------------------------ */
enum { MODE_MANUAL = 0, MODE_WAVE, MODE_BREEZE, MODE_PULSE, MODE_CHASE, MODE_COUNT };

typedef struct
{
    uint32_t magic;
    uint8_t  on[NFAN];        /* Luefter benutzt ja/nein            */
    uint8_t  duty[NFAN];      /* Geschwindigkeit im Modus Manuell %  */
    uint8_t  minp[NFAN];      /* Mindestdrehzahl %                   */
    uint8_t  pos[NFAN];       /* Position im Fenster (fuer Effekte)  */
    char     name[NFAN][16];  /* Name, z.B. "Links gross"            */
    uint8_t  power;           /* Hauptschalter                       */
    uint8_t  mode;            /* Effekt                              */
    uint8_t  master;          /* Gesamtregler %                      */
    uint8_t  lo, hi;          /* Effekt-Bereich %                    */
    uint8_t  ramp;            /* Sanftanlauf: % pro Sekunde, 0 = aus */
    uint16_t period;          /* Effekt-Dauer in 0,1 s               */
    uint16_t freq;            /* PWM-Frequenz in Hz                  */
    uint16_t kick;            /* Anlauf-Kick in ms                   */
    uint32_t sum;             /* Pruefsumme                          */
} Settings;

static Settings S;

/* Laufzeit-Werte (werden nicht gespeichert) */
static volatile uint32_t msTicks;
static float    outPct[NFAN];         /* tatsaechlich ausgegebene % */
static uint32_t kickUntil[NFAN];
static uint32_t sleepEnd;             /* 0 = kein Schlaf-Timer */
static uint32_t sleepStart;
static uint32_t boostEnd;             /* 0 = kein Boost        */
static uint8_t  dirty;                /* Aenderung seit "save" */

void SysTick_Handler(void)
{
    msTicks++;
}

void OTG_HS_IRQHandler(void)
{
    tud_int_handler(0);
}

static uint32_t sumOf(const Settings *s)
{
    const uint8_t *p = (const uint8_t *)s;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < offsetof(Settings, sum); i++)
    {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static void defaults(void)
{
    static const char *names[NFAN] =
    {
        "Links gross 1", "Links gross 2", "Mitte 1", "Mitte 2", "Mitte 3",
        "Mitte 4", "Mitte 5", "Mitte 6", "Rechts gross 1", "Rechts gross 2"
    };
    memset(&S, 0, sizeof(S));
    S.magic = SET_MAGIC;
    for (uint32_t i = 0; i < NFAN; i++)
    {
        S.on[i] = 1;
        S.duty[i] = 100;
        S.minp[i] = 20;
        S.pos[i] = (uint8_t)i;
        strncpy(S.name[i], names[i], 15);
    }
    S.power = 1;
    S.mode = MODE_MANUAL;
    S.master = 100;
    S.lo = 25;
    S.hi = 100;
    S.ramp = 40;
    S.period = 80;
    S.freq = 100;
    S.kick = 400;
}

/* ------------------------------------------------------------------ */
/* Flash: Einstellungen lesen / schreiben (Sektor 7)                   */
/* ------------------------------------------------------------------ */
static void loadSettings(void)
{
    const Settings *f = (const Settings *)SET_ADDR;
    if (f->magic == SET_MAGIC && f->sum == sumOf(f))
    {
        memcpy(&S, f, sizeof(S));
    }
    else
    {
        defaults();
    }
}

static void flashWait(void)
{
    while (FLASH->SR1 & (FLASH_SR_QW | FLASH_SR_BSY))
    {
    }
}

static int saveSettings(void)
{
    static uint32_t buf[(sizeof(Settings) + 31u) / 32u * 8u];
    S.sum = sumOf(&S);
    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, &S, sizeof(S));

    __disable_irq();
    if (FLASH->CR1 & FLASH_CR_LOCK)
    {
        FLASH->KEYR1 = 0x45670123u;
        FLASH->KEYR1 = 0xCDEF89ABu;
    }
    FLASH->CCR1 = FLASH_CCR_CLR_EOP | FLASH_CCR_CLR_WRPERR | FLASH_CCR_CLR_PGSERR |
                  FLASH_CCR_CLR_STRBERR | FLASH_CCR_CLR_INCERR | FLASH_CCR_CLR_OPERR |
                  FLASH_CCR_CLR_RDPERR | FLASH_CCR_CLR_RDSERR | FLASH_CCR_CLR_SNECCERR |
                  FLASH_CCR_CLR_DBECCERR | FLASH_CCR_CLR_CRCEND | FLASH_CCR_CLR_CRCRDERR;

    /* Sektor 7 loeschen */
    FLASH->CR1 = (7u << FLASH_CR_SNB_Pos) | FLASH_CR_SER;
    FLASH->CR1 |= FLASH_CR_START;
    flashWait();
    FLASH->CR1 = 0;

    /* In 32-Byte-Bloecken schreiben */
    volatile uint32_t *dst = (volatile uint32_t *)SET_ADDR;
    for (uint32_t w = 0; w < sizeof(buf) / 4u; w += 8u)
    {
        FLASH->CR1 = FLASH_CR_PG;
        for (uint32_t k = 0; k < 8u; k++)
        {
            dst[w + k] = buf[w + k];
        }
        __DSB();
        flashWait();
        FLASH->CR1 = 0;
    }
    FLASH->CR1 = FLASH_CR_LOCK;
    __enable_irq();

    const Settings *f = (const Settings *)SET_ADDR;
    if (f->magic == SET_MAGIC && f->sum == S.sum)
    {
        dirty = 0;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* PWM                                                                  */
/* ------------------------------------------------------------------ */
static volatile uint32_t *ccr(const FanHw *h)
{
    return &h->tim->CCR1 + (h->ch - 1u);
}

/** Stellt die PWM-Frequenz fuer alle drei Timer ein. */
static void pwmSetFreq(uint32_t hz)
{
    if (hz < 20u)
    {
        hz = 20u;
    }
    if (hz > 25000u)
    {
        hz = 25000u;
    }
    TIM_TypeDef *tims[3] = {TIM2, TIM4, TIM5};
    uint32_t total = TIM_CLK / hz;
    uint32_t psc = total / 65536u;
    uint32_t arr = total / (psc + 1u) - 1u;
    for (uint32_t t = 0; t < 3; t++)
    {
        tims[t]->PSC = psc;
        tims[t]->ARR = arr;
        tims[t]->EGR = TIM_EGR_UG;
    }
}

static void pwmInit(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOBEN | RCC_AHB4ENR_GPIODEN;
    RCC->APB1LENR |= RCC_APB1LENR_TIM2EN | RCC_APB1LENR_TIM4EN | RCC_APB1LENR_TIM5EN;
    (void)RCC->APB1LENR;

    for (uint32_t i = 0; i < NFAN; i++)
    {
        const FanHw *h = &HW[i];
        uint32_t p = h->pin;
        /* Timer-Kanal: PWM-Modus 1 (110) + Vorladen */
        volatile uint32_t *ccmr = (h->ch <= 2u) ? &h->tim->CCMR1 : &h->tim->CCMR2;
        uint32_t sh = ((h->ch - 1u) & 1u) * 8u;
        *ccmr = (*ccmr & ~(0xFFu << sh)) | ((6u << 4) | (1u << 3)) << sh;
        h->tim->CCER |= 1u << ((h->ch - 1u) * 4u);
        *ccr(h) = 0;
        /* Pin dem Timer geben (Alternativ-Funktion) */
        h->port->AFR[p >> 3] = (h->port->AFR[p >> 3] & ~(0xFu << ((p & 7u) * 4u))) |
                               ((uint32_t)h->af << ((p & 7u) * 4u));
        h->port->OTYPER &= ~(1u << p);
        h->port->PUPDR &= ~(3u << (2u * p));
        h->port->OSPEEDR |= (1u << (2u * p));
        h->port->MODER = (h->port->MODER & ~(3u << (2u * p))) | (2u << (2u * p));
    }
    pwmSetFreq(S.freq);
    TIM2->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
    TIM4->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
    TIM5->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
}

static void pwmSet(uint32_t i, float pct)
{
    const FanHw *h = &HW[i];
    uint32_t top = h->tim->ARR + 1u;
    if (pct <= 0.0f)
    {
        *ccr(h) = 0;
    }
    else if (pct >= 100.0f)
    {
        *ccr(h) = top;
    }
    else
    {
        *ccr(h) = (uint32_t)(pct * (float)top / 100.0f);
    }
}

/* ------------------------------------------------------------------ */
/* Effekte: berechnet fuer jeden Luefter die Ziel-Geschwindigkeit      */
/* ------------------------------------------------------------------ */
static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float targetFor(uint32_t i, uint32_t now)
{
    if (!S.power || !S.on[i])
    {
        return 0.0f;
    }
    if (boostEnd != 0u)
    {
        return 100.0f;
    }
    const float TWO_PI = 6.2831853f;
    float t = (float)now / (100.0f * (float)(S.period ? S.period : 1u));
    float p = (float)S.pos[i] / (float)NFAN;
    float v;
    switch (S.mode)
    {
        case MODE_WAVE:
            v = 0.5f + 0.5f * sinf(TWO_PI * (t - p));
            break;
        case MODE_BREEZE:
            v = 0.5f + 0.25f * sinf(TWO_PI * (t * 0.73f + p * 3.1f)) +
                0.15f * sinf(TWO_PI * (t * 1.91f + p * 5.7f)) +
                0.10f * sinf(TWO_PI * (t * 3.70f + p * 13.0f));
            break;
        case MODE_PULSE:
            v = 0.5f + 0.5f * sinf(TWO_PI * t);
            break;
        case MODE_CHASE:
        {
            float d = fabsf((t - floorf(t)) * (float)NFAN - (float)S.pos[i]);
            if (d > NFAN / 2.0f)
            {
                d = (float)NFAN - d;
            }
            v = 1.0f - d / 1.5f;
            break;
        }
        default:
            return (float)S.duty[i] * (float)S.master / 100.0f;
    }
    v = clamp01(v);
    float pct = (float)S.lo + ((float)S.hi - (float)S.lo) * v;
    return pct * (float)S.master / 100.0f;
}

/** Alle 10 ms: Ziele berechnen, Rampe, Mindestdrehzahl, Kick, ausgeben. */
static void fanUpdate(uint32_t now, float dt)
{
    if (sleepEnd != 0u && (int32_t)(now - sleepEnd) >= 0)
    {
        sleepEnd = 0;
        S.power = 0;
        dirty = 1;
    }
    if (boostEnd != 0u && (int32_t)(now - boostEnd) >= 0)
    {
        boostEnd = 0;
    }
    /* Schlaf-Timer: in den letzten 20 % der Zeit langsam leiser werden */
    float sleepScale = 1.0f;
    if (sleepEnd != 0u)
    {
        float total = (float)(sleepEnd - sleepStart);
        float left = (float)(sleepEnd - now);
        if (left < 0.2f * total)
        {
            sleepScale = left / (0.2f * total);
        }
    }
    for (uint32_t i = 0; i < NFAN; i++)
    {
        float tgt = targetFor(i, now) * sleepScale;
        if (tgt > 0.0f && tgt < (float)S.minp[i])
        {
            tgt = (float)S.minp[i];
        }
        if (S.ramp == 0u)
        {
            outPct[i] = tgt;
        }
        else
        {
            float step = (float)S.ramp * dt;
            if (outPct[i] < tgt)
            {
                outPct[i] = (outPct[i] + step > tgt) ? tgt : outPct[i] + step;
            }
            else
            {
                outPct[i] = (outPct[i] - step < tgt) ? tgt : outPct[i] - step;
            }
            if (outPct[i] > 0.0f && outPct[i] < (float)S.minp[i] && tgt > 0.0f)
            {
                outPct[i] = (float)S.minp[i];
            }
            if (tgt == 0.0f && outPct[i] < (float)S.minp[i])
            {
                outPct[i] = 0.0f;
            }
        }
        /* Anlauf-Kick: kurz Vollgas, wenn ein stehender Luefter starten soll */
        static float last[NFAN];
        if (last[i] <= 0.0f && outPct[i] > 0.0f && S.kick > 0u)
        {
            kickUntil[i] = now + S.kick;
        }
        last[i] = outPct[i];
        int kicking = (kickUntil[i] != 0u) && ((int32_t)(now - kickUntil[i]) < 0);
        if (!kicking)
        {
            kickUntil[i] = 0;
        }
        pwmSet(i, kicking ? 100.0f : outPct[i]);
    }
}

/* ------------------------------------------------------------------ */
/* USB-Ausgabe                                                          */
/* ------------------------------------------------------------------ */
static char tx[2048];
static uint32_t txLen;

static void put(const char *s)
{
    while (*s && txLen < sizeof(tx) - 1u)
    {
        tx[txLen++] = *s++;
    }
}

static void putInt(int32_t v)
{
    char b[12];
    int n = 0;
    uint32_t u = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
    do
    {
        b[n++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u && n < 11);
    if (v < 0)
    {
        put("-");
    }
    while (n)
    {
        char c[2] = {b[--n], 0};
        put(c);
    }
}

static void putKV(const char *k, int32_t v, int comma)
{
    put("\"");
    put(k);
    put("\":");
    putInt(v);
    if (comma)
    {
        put(",");
    }
}

/* ------------------------------------------------------------------ */
/* Serielle Leitung zum ESP32: USART1, PA9 = TX, PA10 = RX ("Serial   */
/* Screen"-Stecker), 460800 Baud. Gleiches Text-Protokoll wie USB.    */
/* ------------------------------------------------------------------ */
#define UART_BAUD 460800u
static volatile char     urx[512];       /* Empfangspuffer (Ring)   */
static volatile uint32_t urxHead, urxTail;

void USART1_IRQHandler(void)
{
    uint32_t isr = USART1->ISR;
    if (isr & USART_ISR_RXNE_RXFNE)
    {
        char c = (char)USART1->RDR;
        uint32_t next = (urxHead + 1u) % sizeof(urx);
        if (next != urxTail)
        {
            urx[urxHead] = c;
            urxHead = next;
        }
    }
    if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE))
    {
        USART1->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
    }
}

static void uartInit(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    (void)RCC->APB2ENR;
    /* PA9, PA10 auf Alternativ-Funktion 7 (USART1), PA10 mit Pull-up */
    GPIOA->MODER = (GPIOA->MODER & ~((3u << 18) | (3u << 20))) | (2u << 18) | (2u << 20);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3u << 20)) | (1u << 20);
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~((0xFu << 4) | (0xFu << 8))) | (7u << 4) | (7u << 8);
    USART1->CR1 = 0;
    USART1->BRR = (TIM_CLK + UART_BAUD / 2u) / UART_BAUD;   /* Takt = PCLK2 = 64 MHz */
    USART1->CR1 = USART_CR1_RXNEIE_RXFNEIE | USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
    NVIC_SetPriority(USART1_IRQn, 6);
    NVIC_EnableIRQ(USART1_IRQn);
}

static int uartGet(char *c)
{
    if (urxTail == urxHead)
    {
        return 0;
    }
    *c = urx[urxTail];
    urxTail = (urxTail + 1u) % sizeof(urx);
    return 1;
}

static void uartWrite(const char *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
    {
        while (!(USART1->ISR & USART_ISR_TXE_TXFNF))
        {
        }
        USART1->TDR = (uint8_t)s[i];
    }
}

static void sendTx(void)
{
    uint32_t off = 0;
    uartWrite(tx, txLen);                 /* immer auch an den ESP32 */
    uint32_t guard = msTicks;
    while (off < txLen && tud_cdc_connected())
    {
        uint32_t n = tud_cdc_write_available();
        if (n == 0u)
        {
            tud_cdc_write_flush();
            tud_task();
            if (msTicks - guard > 500u)
            {
                break;
            }
            continue;
        }
        if (n > txLen - off)
        {
            n = txLen - off;
        }
        tud_cdc_write(tx + off, n);
        off += n;
    }
    tud_cdc_write_flush();
    txLen = 0;
}

static void sendState(void)
{
    uint32_t now = msTicks;
    txLen = 0;
    put("{\"t\":\"state\",\"fw\":\"" FW_VERSION "\",");
    putKV("power", S.power, 1);
    putKV("mode", S.mode, 1);
    putKV("master", S.master, 1);
    putKV("lo", S.lo, 1);
    putKV("hi", S.hi, 1);
    putKV("period", S.period, 1);
    putKV("freq", S.freq, 1);
    putKV("kick", S.kick, 1);
    putKV("ramp", S.ramp, 1);
    putKV("sleep", sleepEnd ? (int32_t)((sleepEnd - now) / 1000u) : 0, 1);
    putKV("boost", boostEnd ? (int32_t)((boostEnd - now) / 1000u) : 0, 1);
    putKV("up", (int32_t)(now / 1000u), 1);
    putKV("dirty", dirty, 1);
    put("\"fans\":[");
    for (uint32_t i = 0; i < NFAN; i++)
    {
        put("{\"n\":\"");
        put(S.name[i]);
        put("\",");
        putKV("on", S.on[i], 1);
        putKV("d", S.duty[i], 1);
        putKV("min", S.minp[i], 1);
        putKV("p", S.pos[i], 1);
        int kicking = kickUntil[i] != 0u;
        putKV("o", kicking ? 1000 : (int32_t)(outPct[i] * 10.0f + 0.5f), 0);
        put(i + 1u < NFAN ? "}," : "}");
    }
    put("]}\n");
    sendTx();
}

static void sendMsg(const char *type, const char *msg)
{
    txLen = 0;
    put("{\"t\":\"");
    put(type);
    put("\",\"msg\":\"");
    put(msg);
    put("\"}\n");
    sendTx();
}

/* ------------------------------------------------------------------ */
/* Befehle                                                              */
/* ------------------------------------------------------------------ */
static int32_t clampI(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ------------------------------------------------------------------ */
/* Update ueber den ESP32: Befehl "bootloader" startet den fest       */
/* eingebauten ST-Bootloader (ROM). Der nimmt neue Firmware ueber     */
/* USART1 (PA9/PA10) an - also ueber die Kabel zum ESP32.             */
/* Ablauf: Merker ins Backup-Register, Neustart, gleich nach dem      */
/* Neustart (sauberer Zustand) in den Bootloader springen.            */
/* ------------------------------------------------------------------ */
#define BL_MAGIC 0xB007C0DEu

static void backupAccess(void)
{
    RCC->APB4ENR |= RCC_APB4ENR_RTCAPBEN;
    (void)RCC->APB4ENR;
    PWR->CR1 |= PWR_CR1_DBP;
    while (!(PWR->CR1 & PWR_CR1_DBP))
    {
    }
}

static void bootloaderCheck(void)
{
    RCC->APB4ENR |= RCC_APB4ENR_RTCAPBEN;
    (void)RCC->APB4ENR;
    if (RTC->BKP0R != BL_MAGIC)
    {
        return;
    }
    backupAccess();
    RTC->BKP0R = 0;
    /* Bekannte Startadressen des ST-Bootloaders (H7). Nur springen, wenn dort  */
    /* ein gueltiger Start steht: Stack im RAM, Startadresse im ROM-Bereich.   */
    static const uint32_t cand[] = {0x1FF09800u, 0x1FF0A000u, 0x1FF0A800u};
    uint32_t base = 0;
    for (uint32_t i = 0; i < 3u && !base; i++)
    {
        uint32_t sp = *(volatile uint32_t *)cand[i];
        uint32_t pc = *(volatile uint32_t *)(cand[i] + 4u);
        if ((sp & 0xFF000000u) == 0x20000000u || (sp & 0xFF000000u) == 0x24000000u)
        {
            if ((pc & 0xFFF00000u) == 0x1FF00000u && (pc & 1u))
            {
                base = cand[i];
            }
        }
    }
    if (!base)
    {
        return;                           /* nichts Gueltiges: normal starten */
    }
    SCB->VTOR = base;
    __set_MSP(*(volatile uint32_t *)base);
    ((void (*)(void))(*(volatile uint32_t *)(base + 4u)))();
}

static void enterBootloader(void)
{
    backupAccess();
    RTC->BKP0R = BL_MAGIC;
    for (volatile uint32_t t = 0; t < 2000000u; t++)
    {
    }
    NVIC_SystemReset();
}

static void handleLine(char *line)
{
    char *save = NULL;
    char *cmd = strtok_r(line, " ", &save);
    char *a1 = strtok_r(NULL, " ", &save);
    char *rest = save;
    int32_t v1 = a1 ? atoi(a1) : 0;
    uint32_t now = msTicks;

    if (cmd == NULL)
    {
        return;
    }
    if (!strcmp(cmd, "get"))
    {
        sendState();
        return;
    }
    if (!strcmp(cmd, "bootloader"))
    {
        sendMsg("ok", "bootloader");
        enterBootloader();
    }
    if (!strcmp(cmd, "ping"))
    {
        sendMsg("pong", "Lueftungssystem " FW_VERSION);
        return;
    }
    if (!strcmp(cmd, "save"))
    {
        sendMsg(saveSettings() ? "ok" : "err", "save");
        sendState();
        return;
    }
    if (!strcmp(cmd, "defaults"))
    {
        defaults();
        pwmSetFreq(S.freq);
    }
    else if (!strcmp(cmd, "power"))
    {
        S.power = v1 ? 1 : 0;
        if (!S.power)
        {
            sleepEnd = 0;
            boostEnd = 0;
        }
    }
    else if (!strcmp(cmd, "mode"))
    {
        S.mode = (uint8_t)clampI(v1, 0, MODE_COUNT - 1);
    }
    else if (!strcmp(cmd, "master"))
    {
        S.master = (uint8_t)clampI(v1, 0, 100);
    }
    else if (!strcmp(cmd, "lo"))
    {
        S.lo = (uint8_t)clampI(v1, 0, 100);
    }
    else if (!strcmp(cmd, "hi"))
    {
        S.hi = (uint8_t)clampI(v1, 0, 100);
    }
    else if (!strcmp(cmd, "period"))
    {
        S.period = (uint16_t)clampI(v1, 5, 6000);
    }
    else if (!strcmp(cmd, "freq"))
    {
        S.freq = (uint16_t)clampI(v1, 20, 25000);
        pwmSetFreq(S.freq);
    }
    else if (!strcmp(cmd, "kick"))
    {
        S.kick = (uint16_t)clampI(v1, 0, 3000);
    }
    else if (!strcmp(cmd, "ramp"))
    {
        S.ramp = (uint8_t)clampI(v1, 0, 200);
    }
    else if (!strcmp(cmd, "all"))
    {
        for (uint32_t i = 0; i < NFAN; i++)
        {
            S.duty[i] = (uint8_t)clampI(v1, 0, 100);
        }
    }
    else if (!strcmp(cmd, "sleep"))
    {
        if (v1 <= 0)
        {
            sleepEnd = 0;
        }
        else
        {
            sleepStart = now;
            sleepEnd = now + (uint32_t)clampI(v1, 1, 1440) * 60000u;
            if (sleepEnd == 0u)
            {
                sleepEnd = 1u;
            }
            S.power = 1;
        }
    }
    else if (!strcmp(cmd, "boost"))
    {
        if (v1 <= 0)
        {
            boostEnd = 0;
        }
        else
        {
            boostEnd = now + (uint32_t)clampI(v1, 1, 240) * 60000u;
            if (boostEnd == 0u)
            {
                boostEnd = 1u;
            }
            S.power = 1;
        }
    }
    else if (!strcmp(cmd, "fan") && a1 != NULL)
    {
        int32_t i = v1;
        char *what = strtok_r(NULL, " ", &save);
        char *val = save;
        if (i < 0 || i >= (int32_t)NFAN || what == NULL)
        {
            sendMsg("err", "fan index");
            return;
        }
        int32_t v = (val && *val) ? atoi(val) : 0;
        if (!strcmp(what, "on"))
        {
            S.on[i] = v ? 1 : 0;
        }
        else if (!strcmp(what, "duty"))
        {
            S.duty[i] = (uint8_t)clampI(v, 0, 100);
        }
        else if (!strcmp(what, "min"))
        {
            S.minp[i] = (uint8_t)clampI(v, 0, 100);
        }
        else if (!strcmp(what, "pos"))
        {
            S.pos[i] = (uint8_t)clampI(v, 0, NFAN - 1);
        }
        else if (!strcmp(what, "name") && val)
        {
            memset(S.name[i], 0, 16);
            for (uint32_t k = 0; k < 15u && val[k]; k++)
            {
                char c = val[k];
                S.name[i][k] = (c == '"' || c == '\\' || c < 32) ? '_' : c;
            }
        }
        else
        {
            sendMsg("err", "fan option");
            return;
        }
    }
    else
    {
        (void)rest;
        sendMsg("err", "unknown command");
        return;
    }
    dirty = 1;
    sendState();
}

/* ------------------------------------------------------------------ */
/* Start                                                                */
/* ------------------------------------------------------------------ */
static void powerInit(void)
{
    /* Interner Spannungsregler (LDO), danach warten bis bereit */
    MODIFY_REG(PWR->CR3, PWR_CR3_SCUEN | PWR_CR3_LDOEN | PWR_CR3_BYPASS, PWR_CR3_LDOEN);
    for (uint32_t t = 0; t < 1000000u && !(PWR->CSR1 & PWR_CSR1_ACTVOSRDY); t++)
    {
    }
}

static void usbInit(void)
{
    /* 48-MHz-USB-Takt aus HSI48, nachgeregelt durch CRS am USB-Signal */
    RCC->CR |= RCC_CR_HSI48ON;
    while (!(RCC->CR & RCC_CR_HSI48RDY))
    {
    }
    MODIFY_REG(RCC->D2CCIP2R, RCC_D2CCIP2R_USBSEL, RCC_D2CCIP2R_USBSEL_0 | RCC_D2CCIP2R_USBSEL_1);
    RCC->APB1HENR |= RCC_APB1HENR_CRSEN;
    (void)RCC->APB1HENR;
    MODIFY_REG(CRS->CFGR, CRS_CFGR_SYNCSRC, CRS_CFGR_SYNCSRC_1);
    CRS->CR |= CRS_CR_AUTOTRIMEN | CRS_CR_CEN;

    /* USB-Spannungsregler */
    PWR->CR3 |= PWR_CR3_USB33DEN;
    for (uint32_t t = 0; t < 1000000u && !(PWR->CR3 & PWR_CR3_USB33RDY); t++)
    {
    }

    /* PA11 = D-, PA12 = D+  (Alternativ-Funktion 10) */
    GPIOA->MODER = (GPIOA->MODER & ~((3u << 22) | (3u << 24))) | (2u << 22) | (2u << 24);
    GPIOA->OSPEEDR |= (3u << 22) | (3u << 24);
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~((0xFu << 12) | (0xFu << 16))) | (10u << 12) | (10u << 16);

    RCC->AHB1ENR |= RCC_AHB1ENR_USB1OTGHSEN;
    RCC->AHB1LPENR &= ~RCC_AHB1LPENR_USB1OTGHSULPILPEN;
    (void)RCC->AHB1ENR;
    NVIC_SetPriority(OTG_HS_IRQn, 5);

    tud_configure_dwc2_t cfg = CFG_TUD_CONFIGURE_DWC2_DEFAULT;
    cfg.vbus_sensing = false;
    tud_configure(0, TUD_CFGID_DWC2, &cfg);
    tusb_rhport_init_t init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
    tusb_rhport_init(0, &init);
}

int main(void)
{
    bootloaderCheck();                    /* Update-Modus angefordert? */
    powerInit();
    SystemCoreClockUpdate();
    SysTick_Config(SystemCoreClock / 1000u);
    loadSettings();
    pwmInit();
    usbInit();
    uartInit();

    static char line[160];
    static char uline[160];               /* Zeilen vom ESP32 */
    uint32_t len = 0;
    uint32_t ulen = 0;
    uint32_t lastUpd = msTicks;
    uint32_t lastPush = msTicks;

    for (;;)
    {
        tud_task();

        /* Empfangene Zeichen zu Zeilen sammeln */
        while (tud_cdc_available())
        {
            char c;
            if (tud_cdc_read(&c, 1) != 1)
            {
                break;
            }
            if (c == '\n' || c == '\r')
            {
                if (len)
                {
                    line[len] = 0;
                    handleLine(line);
                    len = 0;
                }
            }
            else if (len < sizeof(line) - 1u)
            {
                line[len++] = c;
            }
        }

        /* Zeilen vom ESP32 (WLAN / Bluetooth) */
        char uc;
        while (uartGet(&uc))
        {
            if (uc == '\n' || uc == '\r')
            {
                if (ulen)
                {
                    uline[ulen] = 0;
                    handleLine(uline);
                    ulen = 0;
                }
            }
            else if (ulen < sizeof(uline) - 1u)
            {
                uline[ulen++] = uc;
            }
        }

        uint32_t now = msTicks;
        if (now - lastUpd >= 10u)
        {
            fanUpdate(now, (float)(now - lastUpd) / 1000.0f);
            lastUpd = now;
        }
        /* Live-Status 5x pro Sekunde an die App */
        if (now - lastPush >= 200u)       /* USB und ESP32 */
        {
            lastPush = now;
            sendState();
        }
    }
}
