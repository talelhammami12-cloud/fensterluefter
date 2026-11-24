/**
 * @file    startup.c
 * @brief   Minimaler Start-Code fuer den STM32H723 (ersetzt die startup_*.s).
 *
 * Nach dem Reset liest der Prozessor:
 *   Adresse 0x08000000 -> Startwert fuer den Stack-Pointer (SP)
 *   Adresse 0x08000004 -> Adresse von Reset_Handler (PC)
 * Reset_Handler kopiert die Startwerte der Variablen ins RAM, setzt den
 * Rest auf 0 und ruft dann main() auf.
 */

#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

int main(void);

/** Wird nach jedem Reset ausgefuehrt. */
void Reset_Handler(void)
{
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;

    /* Startwerte der Variablen aus dem Flash ins RAM kopieren */
    while (dst < &_edata)
    {
        *dst++ = *src++;
    }

    /* Variablen ohne Startwert auf 0 setzen */
    for (dst = &_sbss; dst < &_ebss; dst++)
    {
        *dst = 0u;
    }

    main();

    for (;;)
    {
    }
}

/** Fuer alle Fehler: einfach stehen bleiben. */
void Default_Handler(void)
{
    for (;;)
    {
    }
}

/** Vektortabelle: SP-Startwert, Reset und die Fehler-Handler. */
__attribute__((section(".isr_vector"), used))
void (* const vectorTable[16])(void) =
{
    (void (*)(void))(&_estack),  /* 0x00: Stack-Pointer          */
    Reset_Handler,               /* 0x04: Reset                  */
    Default_Handler,             /* 0x08: NMI                    */
    Default_Handler,             /* 0x0C: HardFault              */
    Default_Handler,             /* 0x10: MemManage              */
    Default_Handler,             /* 0x14: BusFault               */
    Default_Handler,             /* 0x18: UsageFault             */
    0, 0, 0, 0,                  /* 0x1C ... 0x28: reserviert     */
    Default_Handler,             /* 0x2C: SVCall                 */
    Default_Handler,             /* 0x30: DebugMon               */
    0,                           /* 0x34: reserviert             */
    Default_Handler,             /* 0x38: PendSV                 */
    Default_Handler              /* 0x3C: SysTick                */
};
