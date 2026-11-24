@echo off
rem Baut die Firmware neu (z.B. nachdem du SUPPLY_PERCENT geaendert hast).
rem Ergebnis: fensterluefter.hex  -> mit STM32CubeProgrammer aufspielen.

set GCCDIR=C:\ST\STM32CubeIDE_2.1.1\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740\tools\bin
cd /d "%~dp0"

"%GCCDIR%\arm-none-eabi-gcc.exe" -mcpu=cortex-m7 -mthumb -mfloat-abi=soft -Os ^
  -Wall -Wextra -ffreestanding -fno-tree-loop-distribute-patterns ^
  -nostdlib -nostartfiles -T link.ld main.c startup.c -o fensterluefter.elf
if errorlevel 1 goto fehler

"%GCCDIR%\arm-none-eabi-objcopy.exe" -O ihex fensterluefter.elf fensterluefter.hex
"%GCCDIR%\arm-none-eabi-size.exe" fensterluefter.elf
echo.
echo Fertig: fensterluefter.hex
pause
exit /b 0

:fehler
echo.
echo FEHLER beim Bauen - siehe Meldungen oben.
pause
exit /b 1
