@echo off
rem Baut die USB-Firmware -> lueftungssystem_usb.hex (mit STM32CubeProgrammer aufspielen)
set GCCDIR=C:\ST\STM32CubeIDE_2.1.1\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740\tools\bin
set LIB=C:\Users\talel\fwlibs
set TU=%LIB%\tinyusb\src
cd /d "%~dp0"

"%GCCDIR%\arm-none-eabi-gcc.exe" -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -Os -g0 ^
  -DSTM32H723xx -DCFG_TUSB_MCU=OPT_MCU_STM32H7 -Wall -ffunction-sections -fdata-sections ^
  -I. -I"%LIB%\cmsis-device-h7\Include" -I"%LIB%\CMSIS_5\CMSIS\Core\Include" -I"%TU%" ^
  main.c usb_descriptors.c system_stm32h7xx.c startup_stm32h723xx.s ^
  "%TU%\tusb.c" "%TU%\common\tusb_fifo.c" "%TU%\device\usbd.c" ^
  "%TU%\class\cdc\cdc_device.c" "%TU%\portable\synopsys\dwc2\dcd_dwc2.c" "%TU%\portable\synopsys\dwc2\dwc2_common.c" ^
  -T link.ld --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -lm -o lueftungssystem_usb.elf
if errorlevel 1 goto fehler
"%GCCDIR%\arm-none-eabi-objcopy.exe" -O ihex lueftungssystem_usb.elf lueftungssystem_usb.hex
"%GCCDIR%\arm-none-eabi-objcopy.exe" -O binary lueftungssystem_usb.elf lueftungssystem_usb.bin
"%GCCDIR%\arm-none-eabi-size.exe" lueftungssystem_usb.elf
echo Fertig: lueftungssystem_usb.hex
exit /b 0
:fehler
echo FEHLER beim Bauen
exit /b 1
