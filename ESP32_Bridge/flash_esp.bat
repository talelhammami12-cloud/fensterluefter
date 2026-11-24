@echo off
rem Spielt das ESP32-Programm auf. ESP32 per USB-Kabel am PC anschliessen.
rem Optional: flash_esp.bat COM5
set CLI=%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe
cd /d "%~dp0"
set PORT=%1
if "%PORT%"=="" for /f "tokens=1" %%p in ('call "%CLI%" board list ^| findstr /i "COM"') do if not defined PORT set PORT=%%p
if "%PORT%"=="" (
  echo Kein ESP32 gefunden. USB-Kabel pruefen, dann nochmal starten.
  pause
  exit /b 1
)
echo ESP32 an %PORT% - wird aufgespielt...
"%CLI%" upload -p %PORT% --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs --input-dir build . || goto fehler
echo Fertig. ESP32 startet neu, WLAN "Lueftungssystem" erscheint.
pause
exit /b 0
:fehler
echo FEHLER. Falls es haengt: BOOT-Taste am ESP32 gedrueckt halten, bis "Connecting" weiterlaeuft.
pause
exit /b 1
