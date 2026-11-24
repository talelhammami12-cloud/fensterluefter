@echo off
rem Baut das ESP32-Programm (WLAN + Bluetooth Bruecke)
set CLI=%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe
cd /d "%~dp0"
python make_html.py || goto fehler
"%CLI%" compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs --output-dir build . || goto fehler
echo Fertig: build\ESP32_Bridge.ino.merged.bin
exit /b 0
:fehler
echo FEHLER beim Bauen
exit /b 1
