@echo off
rem Spielt das ESP32-Programm ueber WLAN auf (kein USB-Kabel noetig).
rem PC muss im gleichen WLAN wie die Lueftung sein. Optional: ota_esp.bat 192.168.1.50
cd /d "%~dp0"
call secrets.bat
set HOST=%1
if "%HOST%"=="" set HOST=lueftung.local
if not exist build\ESP32_Bridge.ino.bin (
  echo Erst build_esp.bat ausfuehren.
  pause
  exit /b 1
)
echo Lade Firmware auf http://%HOST%/update ...
curl --fail -s -H "Origin: http://%HOST%" -u admin:%OTA_PASS% -F "update=@build\ESP32_Bridge.ino.bin" http://%HOST%/update || goto fehler
echo.
echo Fertig. Die Lueftung startet neu (ca. 10 Sekunden).
pause
exit /b 0
:fehler
echo FEHLER: Lueftung nicht erreichbar. Gleiches WLAN? Sonst IP-Adresse angeben: ota_esp.bat 192.168.x.x
pause
exit /b 1
