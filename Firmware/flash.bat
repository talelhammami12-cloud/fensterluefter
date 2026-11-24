@echo off
rem Spielt fensterluefter.hex auf das FLY-Super8 Pro (USB, DFU-Modus).
rem Vorher: BOOT-Taste gedrueckt halten, Board per USB-C einstecken
rem (oder RESET druecken), BOOT loslassen. Dann diese Datei doppelklicken.

set CLI="C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe"
cd /d "%~dp0"

%CLI% -c port=USB1 -e all -w fensterluefter.hex -v -rst
if errorlevel 1 goto fehler

echo.
echo Fertig - alle Luefter sollten jetzt laufen.
pause
exit /b 0

:fehler
echo.
echo FEHLER: Board nicht im DFU-Modus gefunden oder Flashen fehlgeschlagen.
echo BOOT halten, RESET kurz druecken, BOOT loslassen - dann nochmal.
pause
exit /b 1
