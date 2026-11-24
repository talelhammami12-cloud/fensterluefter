/**
 * @file    fan4test.c
 * @brief   Test: NUR FAN4 (PA15) an, alle anderen 9 Anschluesse AUS.
 *
 * Laufen danach die anderen Luefter nicht mehr, dann laeuft diese
 * Firmware wirklich. Bleibt FAN4 trotzdem bei 0 V, ist der Anschluss
 * auf dem Board defekt. Dreht der FAN4-Luefter, lag es am Code.
 */
#include <stdint.h>

#define REG(addr)       (*(volatile uint32_t *)(addr))
#define RCC_AHB4ENR     REG(0x58024400u + 0x0E0u)
#define GPIOA_BASE      0x58020000u
#define GPIOB_BASE      0x58020400u
#define GPIOD_BASE      0x58020C00u
#define GPIO_MODER(b)   REG((b) + 0x00u)
#define GPIO_OTYPER(b)  REG((b) + 0x04u)
#define GPIO_PUPDR(b)   REG((b) + 0x0Cu)
#define GPIO_BSRR(b)    REG((b) + 0x18u)

/** Pin als normalen Push-Pull-Ausgang ohne Pull-Widerstand einstellen. */
static void makeOutput(uint32_t base, uint32_t pin)
{
    GPIO_OTYPER(base) &= ~(1u << pin);
    GPIO_PUPDR(base)  &= ~(3u << (2u * pin));
    GPIO_MODER(base)  &= ~(3u << (2u * pin));
    GPIO_MODER(base)  |=  (1u << (2u * pin));
}

int main(void)
{
    RCC_AHB4ENR |= (1u << 0) | (1u << 1) | (1u << 3);
    (void)RCC_AHB4ENR;

    /* Alle Anschluesse ausser FAN4 auf LOW = aus */
    GPIO_BSRR(GPIOA_BASE) = (1u << (0 + 16)) | (1u << (1 + 16)) |
                            (1u << (2 + 16)) | (1u << (3 + 16));
    GPIO_BSRR(GPIOB_BASE) = (1u << (10 + 16)) | (1u << (11 + 16));
    GPIO_BSRR(GPIOD_BASE) = (1u << (12 + 16)) | (1u << (14 + 16)) |
                            (1u << (15 + 16));
    makeOutput(GPIOA_BASE, 0u);  makeOutput(GPIOA_BASE, 1u);
    makeOutput(GPIOA_BASE, 2u);  makeOutput(GPIOA_BASE, 3u);
    makeOutput(GPIOB_BASE, 10u); makeOutput(GPIOB_BASE, 11u);
    makeOutput(GPIOD_BASE, 12u); makeOutput(GPIOD_BASE, 14u);
    makeOutput(GPIOD_BASE, 15u);

    /* FAN4 = PA15 auf HIGH = an */
    GPIO_BSRR(GPIOA_BASE) = (1u << 15);
    makeOutput(GPIOA_BASE, 15u);

    for (;;)
    {
        GPIO_BSRR(GPIOA_BASE) = (1u << 15);
    }
}
