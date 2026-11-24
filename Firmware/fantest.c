/**
 * @file    fantest.c
 * @brief   Test-Firmware: schaltet FAN0 ... FAN9 nacheinander EINZELN ein.
 *
 * Zuerst: ALLE an fuer 5 s (zeigt: die Firmware laeuft).
 * Dann (wiederholt sich endlos):
 *   alle aus 1 s -> nur FAN0 an ca. 4 s -> alle aus 1 s -> nur FAN1 an ...
 *   ... -> nur FAN9 -> dann ca. 5 s ALLE an -> von vorne.
 * So sieht man genau, welcher Anschluss welchen Luefter schaltet und
 * welcher Anschluss nichts tut.
 */

#include <stdint.h>

#define REG(addr)       (*(volatile uint32_t *)(addr))
#define RCC_AHB4ENR     REG(0x58024400u + 0x0E0u)
#define GPIOA_BASE      0x58020000u
#define GPIOB_BASE      0x58020400u
#define GPIOD_BASE      0x58020C00u
#define GPIO_MODER(b)   REG((b) + 0x00u)
#define GPIO_BSRR(b)    REG((b) + 0x18u)

/** Ein Anschluss = Port + Pin-Nummer. */
typedef struct
{
    uint32_t base;
    uint32_t pin;
} FanPin;

static const FanPin fans[10] =
{
    {GPIOA_BASE, 0u},  {GPIOA_BASE, 1u},  {GPIOA_BASE, 2u},
    {GPIOA_BASE, 3u},  {GPIOA_BASE, 15u}, {GPIOB_BASE, 11u},
    {GPIOB_BASE, 10u}, {GPIOD_BASE, 12u}, {GPIOD_BASE, 14u},
    {GPIOD_BASE, 15u}
};

/* SysTick: Zaehler im Prozessor, laeuft mit dem 64-MHz-Grundtakt */
#define SYST_CSR        REG(0xE000E010u)
#define SYST_RVR        REG(0xE000E014u)
#define SYST_CVR        REG(0xE000E018u)

/** Wartet genau ms Millisekunden (SysTick zaehlt 64000 Takte = 1 ms). */
static void waitMs(uint32_t ms)
{
    SYST_RVR = 64000u - 1u;
    SYST_CVR = 0u;
    SYST_CSR = 5u;                 /* Prozessortakt, Zaehler an */
    while (ms > 0u)
    {
        /* Bit 16 (COUNTFLAG) wird 1, wenn 1 ms vorbei ist */
        if ((SYST_CSR & (1u << 16)) != 0u)
        {
            ms--;
        }
    }
}

/** Setzt einen Anschluss an (1) oder aus (0). */
static void fanSet(uint32_t n, uint32_t on)
{
    GPIO_BSRR(fans[n].base) = on ? (1u << fans[n].pin)
                                 : (1u << (fans[n].pin + 16u));
}

int main(void)
{
    RCC_AHB4ENR |= (1u << 0) | (1u << 1) | (1u << 3);
    (void)RCC_AHB4ENR;

    /* Alle 10 Pins aus und als Ausgang einstellen */
    for (uint32_t n = 0u; n < 10u; n++)
    {
        fanSet(n, 0u);
        GPIO_MODER(fans[n].base) &= ~(3u << (2u * fans[n].pin));
        GPIO_MODER(fans[n].base) |=  (1u << (2u * fans[n].pin));
    }

    /* Start: alle 5 s an */
    for (uint32_t n = 0u; n < 10u; n++)
    {
        fanSet(n, 1u);
    }
    waitMs(5000u);
    for (uint32_t n = 0u; n < 10u; n++)
    {
        fanSet(n, 0u);
    }

    for (;;)
    {
        /* Jeden Anschluss einzeln einschalten */
        for (uint32_t n = 0u; n < 10u; n++)
        {
            waitMs(1000u);
            fanSet(n, 1u);
            waitMs(4000u);
            fanSet(n, 0u);
        }

        /* Zum Schluss alle zusammen */
        for (uint32_t n = 0u; n < 10u; n++)
        {
            fanSet(n, 1u);
        }
        waitMs(5000u);
        for (uint32_t n = 0u; n < 10u; n++)
        {
            fanSet(n, 0u);
        }
        waitMs(3000u);
    }
}
