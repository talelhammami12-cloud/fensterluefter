@echo off
rem Spielt die FLY-Firmware ueber WLAN auf (ueber den ESP32, kein USB, kein BOOT-Jumper).
rem PC muss im gleichen WLAN wie die Lueftung sein. Optional: fly_ota.bat 192.168.1.50
cd /d "%~dp0"
call ..\ESP32_Bridge\secrets.bat
set HOST=%1
if "%HOST%"=="" set HOST=lueftung.local
if not exist lueftungssystem_usb.bin (
  echo Erst build.bat ausfuehren.
  pause
  exit /b 1
)
echo FLY-Firmware wird ueber http://%HOST%/flyupdate aufgespielt (ca. 20 Sekunden) ...
curl --fail -s -u admin:%OTA_PASS% -F "fw=@lueftungssystem_usb.bin" http://%HOST%/flyupdate || goto fehler
echo.
pause
exit /b 0
:fehler
echo FEHLER: Lueftung nicht erreichbar. Gleiches WLAN? Sonst IP-Adresse angeben: fly_ota.bat 192.168.x.x
pause
exit /b 1
