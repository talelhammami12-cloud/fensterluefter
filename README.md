# Lüftungssystem – window ventilation panel

A 3D-printed ventilation panel that sits in a window opening, with 11 × 12 V fans,
controlled by a FLY-Super8 Pro board (STM32H723) and an ESP32 for WiFi, Bluetooth
and control from anywhere.

## What is in here

| Folder | Content |
|---|---|
| `Fusion/`, `*.f3d`, `*.step`, `*.3mf` | CAD (Fusion 360) and print files |
| `STL/` | Single parts for printing (incl. the top twist latches) |
| `Firmware_USB/` | FLY board firmware (bare-metal C, STM32H723): fan PWM, settings, USB + UART protocol |
| `ESP32_Bridge/` | ESP32 firmware: open WiFi with captive-portal app, home WiFi, Bluetooth LE, Internet relay (MQTT), over-the-air updates for itself and the FLY board |
| `App/` | The control app (one HTML file, Web Serial / WiFi / Bluetooth / Internet) |
| `Bilder/`, `Bauanleitung.html` | Build instructions with pictures |
| `*.pdf`, `Schaltplan.svg` | Wiring diagrams |

## Private keys

Passwords and the Internet key are **not** in this repo. Copy the example files and fill in your own:

- `ESP32_Bridge/secrets.example.h` → `secrets.h`
- `ESP32_Bridge/secrets.example.bat` → `secrets.bat`
- `App/secrets.example.js` → `secrets.js`

## Build and update

- FLY: `Firmware_USB/build.bat`, then first time by USB with STM32CubeProgrammer, later `fly_ota.bat` over WiFi.
- ESP32: `ESP32_Bridge/build_esp.bat`, first time `flash_esp.bat` by USB, later `ota_esp.bat` over WiFi.

Ing. T. Hammami
