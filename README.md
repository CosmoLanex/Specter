<div align="center">

<img src="https://github.com/CosmoLanex/Specter/blob/main/assets/lg.png?raw=true" width="600"/>

# SPECTER
### 2.4 GHz RF Intelligence System

[![License: MIT](https://img.shields.io/badge/License-MIT-red.svg)](https://opensource.org/licenses/MIT)
[![Platform: ESP32](https://img.shields.io/badge/Platform-ESP32-blue.svg)](https://www.espressif.com/)
[![Framework: Arduino](https://img.shields.io/badge/Framework-Arduino-teal.svg)](https://www.arduino.cc/)
[![Status: Active](https://img.shields.io/badge/Status-Active-brightgreen.svg)]()
[![Made in India](https://img.shields.io/badge/Made%20in-India-orange.svg)]()

> **NUKRAX v3.0** — A dual-ESP32 RF intelligence platform featuring 2.4 GHz spectrum scanning, WiFi/BLE surveillance, proximity alerting and active RF disruption via twin NRF24L01+PA/LNA modules.

**Developed by [@CosmoLanex](https://github.com/CosmoLanex)**

---

[🔍 Features](#features) · [🔧 Hardware](#hardware) · [📡 Circuit Diagram](#circuit-diagram) · [💾 Installation](#installation) · [🎮 Usage](#usage) · [📁 Repository](#repository-structure) · [❤️ Support](#support-the-creator)

</div>

---

## ⚠️ Disclaimer

This project is developed for **educational and research purposes only**. The RF disruption feature (JR.IO) transmits a constant 2.4 GHz carrier wave and may interfere with WiFi, Bluetooth, and other 2.4 GHz communications in the vicinity. Usage of such functionality may be **regulated or illegal in your jurisdiction**. The developer assumes no responsibility for misuse. Always operate within the bounds of your local laws and regulations.

---

## Overview

**Specter** is a dual-ESP32 RF intelligence platform built around two ESP32-WROOM-32 modules and a pair of NRF24L01+PA/LNA transceiver modules. The system provides a fully self-contained handheld interface — controlled via three push buttons and a 0.96" OLED display — capable of:

- Scanning and displaying nearby **WiFi networks** with SSID, RSSI, channel and encryption
- Scanning nearby **Bluetooth Low Energy (BLE)** devices
- Performing a **passive 2.4 GHz spectrum sweep** across all 126 NRF24 channels simultaneously using two radios
- Broadcasting **custom alert messages** to all nearby devices via WiFi SSID and BLE beacon — no connection required
- Running **JR.IO**, a dual-radio 2.4 GHz constant-carrier disruptor in sweep or random-hop mode

The system is split across two ESP32s: **ESP1** handles all RF work (NRF24 scanning, WiFi scan, BLE scan, alerts, JR.IO) and **ESP2** handles the user interface (OLED display, buttons, menu navigation). They communicate over a direct hardware UART link.

---

## Features

### 🎬 NUKRAX Boot Sequence
A high-tech 4-phase glitch animation plays on boot:
- **Static burst** — random noise fills the screen
- **Text emergence** — "NUKRAX" glitches in through noise
- **Active glitch hold** — HUD corner markers, scan lines, blinking cursor, subtitle flicker
- **Dissolve out** — text breaks apart, screen wipes to black

### 📡 JR.IO — 2.4 GHz Disruptor *(Main Feature)*
- Both NRF24L01+PA/LNA modules output a **continuous unmodulated 2.4 GHz carrier** at maximum power (+20 dBm)
- **Slide switch** on ESP1 selects mode live — no restart needed:
  - `Switch HIGH` → **Sweep mode** — HSPI sweeps ±2 channels, VSPI sweeps ±4, staggered for maximum band coverage
  - `Switch LOW` → **Random hop** — both radios jump to independent random channels every ~60 µs
- Runs in a **zero-delay tight loop** for maximum disruption efficiency

### 📶 WiFi Scanner
- Scans all nearby networks including **hidden SSIDs**
- Displays SSID, channel, RSSI in dBm, encryption status, BSSID
- Results sorted by signal strength
- Scrollable list with detail view

### 🔵 BLE Scanner
- 6-second passive BLE advertisement scan
- Displays device name, MAC address, RSSI
- Identifies unnamed advertisement-only devices

### 📊 NRF24 Spectrum Analyser
- Both NRF24 radios sweep simultaneously — HSPI covers channels 0–62, VSPI covers 63–125
- Full 2.4 GHz band visualised as a **bar graph spectrum** on OLED
- WiFi channel 1, 6, 11 marked on spectrum display
- Pannable view with UP/DOWN buttons

### 🚨 Alert System — No Connection Required
Sends alert messages to ALL nearby devices automatically:

| Method | How it reaches phones | Range |
|---|---|---|
| **WiFi SSID Only** | Alert message IS the WiFi network name — visible in every phone's WiFi list | ~100m |
| **BLE Beacon Only** | Alert message IS the BLE device name — broadcasts 50 packets/sec | ~50m |
| **Blast All** | Both simultaneously at maximum TX power | ~100m |

Custom messages can be composed character by character on the device itself.

---

## Hardware

### Components Required

| Component | Quantity | Notes |
|---|---|---|
| ESP32-WROOM-32 Dev Module | 2 | Any standard ESP32 dev board |
| NRF24L01+PA/LNA Module | 2 | Must be PA/LNA version for maximum range |
| 0.96" I2C OLED Display (SSD1306) | 1 | 4-pin I2C, 128×64, connected to ESP2 |
| Push Button | 3 | UP, DOWN, MIDDLE — 6×6mm tactile |
| Slide Switch | 1 | 3-pin, connected to ESP1 GPIO33 |
| Jumper Wires | — | Male-to-male and male-to-female |
| Breadboard or PCB | — | For assembly |
| 100µF Capacitors | 2 | One per NRF24 module for power stability |

### NRF24L01+PA/LNA Module
<img src="https://github.com/CosmoLanex/Specter/blob/main/assets/nrf24l01.jpg?raw=true" width="300"/>

> ⚠️ **Important:** Use the PA/LNA version (with external antenna), not the basic onboard-antenna version. The 100µF capacitor across VCC/GND of each module is strongly recommended to prevent brown-out resets.

### ESP32-WROOM-32
The project uses two standard ESP32-WROOM-32 dev modules. Any 38-pin ESP32 dev board with the standard pinout works.

### 0.96" OLED Display
4-pin I2C SSD1306 module. Blue or white color — both work identically with the code.

---

## Circuit Diagram

### Full Circuit
[![Circuit Diagram](https://github.com/CosmoLanex/Specter/blob/f79d2e7878d860163c0b8690aaf28b992a85a15c/circuit-diagram/main-c.d.png?raw=true)](https://app.cirkitdesigner.com/project/870d9182-9367-47eb-bef0-2a379b8e2756)

> 🔗 **Interactive circuit diagram:** [Open in Cirkit Designer](https://app.cirkitdesigner.com/project/870d9182-9367-47eb-bef0-2a379b8e2756)

---

### ESP1 Pinout

| Pin | Connection |
|---|---|
| GPIO 14 | NRF24 #1 SCK (HSPI) |
| GPIO 12 | NRF24 #1 MISO (HSPI) |
| GPIO 13 | NRF24 #1 MOSI (HSPI) |
| GPIO 15 | NRF24 #1 CS (HSPI) |
| GPIO 16 | NRF24 #1 CE (HSPI) |
| GPIO 18 | NRF24 #2 SCK (VSPI) |
| GPIO 19 | NRF24 #2 MISO (VSPI) |
| GPIO 23 | NRF24 #2 MOSI (VSPI) |
| GPIO 21 | NRF24 #2 CS (VSPI) |
| GPIO 22 | NRF24 #2 CE (VSPI) |
| GPIO 33 | Slide Switch |
| GPIO 17 | UART TX → ESP2 RX |
| GPIO 32 | UART RX ← ESP2 TX |

### ESP2 Pinout

| Pin | Connection |
|---|---|
| GPIO 4 | OLED SDA |
| GPIO 5 | OLED SCL |
| GPIO 25 | Button UP |
| GPIO 27 | Button DOWN |
| GPIO 26 | Button MIDDLE (SELECT) |
| GPIO 32 | UART TX → ESP1 RX |
| GPIO 17 | UART RX ← ESP1 TX |

### UART Cross-Wiring
```
ESP1 GPIO17 (TX) ────────→ ESP2 GPIO17 (RX)
ESP2 GPIO32 (TX) ────────→ ESP1 GPIO32 (RX)
ESP1 GND         ──────── ESP2 GND   ← mandatory
```

---

## Installation

### 1. Install Arduino IDE
Download and install [Arduino IDE 1.8.x or 2.x](https://www.arduino.cc/en/software)

### 2. Install ESP32 Board Package
1. Open Arduino IDE → **File → Preferences**
2. Add to Additional Boards Manager URLs:
```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```
3. Go to **Tools → Board → Boards Manager**
4. Search `esp32` and install **esp32 by Espressif Systems**

### 3. Install Required Libraries

Open **Sketch → Include Library → Manage Libraries** and install:

| Library | Author | For |
|---|---|---|
| `Adafruit SSD1306` | Adafruit | OLED display (ESP2) |
| `Adafruit GFX Library` | Adafruit | OLED graphics (ESP2) |
| `RF24` | TMRh20 | NRF24L01 modules (ESP1) |
| `ezButton` | ArduinoGetStarted | Slide switch (ESP1) |

> WiFi, BLE, SPI, Wire are built into the ESP32 board package — no installation needed.

### 4. Flash ESP1
1. Open `specter-flash-ino/esp1_worker.ino`
2. Select **Tools → Board → ESP32 Dev Module**
3. Set **Tools → Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)**
4. Select the correct COM port
5. Click Upload
6. Open Serial Monitor at 115200 baud — confirm `HP Started !!!` and `SP Started !!!`

### 5. Flash ESP2
1. Open `specter-flash-ino/esp2_controller.ino`
2. Select **Tools → Board → ESP32 Dev Module**
3. Select the correct COM port
4. Click Upload

---

## Usage

### Boot
Power on both ESPs. The OLED on ESP2 displays the **NUKRAX glitch animation** for ~4 seconds then the main menu appears.

### Navigation

| Button | Action |
|---|---|
| UP | Move cursor up / scroll list up / cycle character |
| DOWN | Move cursor down / scroll list down / cycle character |
| MIDDLE (short press) | Select / confirm / activate |
| MIDDLE (hold 1s) | Go back to previous screen |

### Main Menu

```
> JR.IO MODE    ← Main feature
  WIFI  SCAN
  BLE   SCAN
  NRF   SCAN
  ALERT MODE
```

### JR.IO Mode
1. Select `JR.IO MODE` → press MIDDLE to activate
2. Both NRF24 radios start constant carrier at MAX power
3. **Slide switch on ESP1** controls mode:
   - `HIGH (open)` = Sweep — staggered channel sweep
   - `LOW (GND)` = Random — random channel hop every ~60µs
4. Press MIDDLE to stop

### WiFi / BLE / NRF Scan
1. Select scan type → press MIDDLE to activate
2. Scanning animation plays while ESP1 works
3. Results appear — UP/DOWN scrolls, MIDDLE opens action menu
4. Action menu: **Scan Again / Deactivate / Back to Menu**

### Alert Mode
1. Select `ALERT MODE` → press MIDDLE
2. Choose: **Preset Message** or **Custom Message**
3. Choose method: **WiFi SSID / BLE Beacon / Blast All**
4. Press MIDDLE to launch — alert broadcasts automatically
5. Press MIDDLE to stop

---

## Repository Structure

```
Specter/
├── assets/
│   ├── lg.png               ← Project logo
│   └── nrf24l01.jpg         ← Component image
├── circuit-diagram/
│   ├── main-c.d.png         ← Full circuit diagram
│   └── ...
├── specter-flash-ino/
│   ├── esp1_worker.ino      ← Flash to ESP1
│   ├── esp2_controller.ino  ← Flash to ESP2
│   └── other-menu/
│       └── jammer.ino       ← Original JR.IO code (more.ino)
├── specter/flash-code/
│   ├── esp32-no1.ino
│   └── esp32-no2.ino
└── README.md
```

---

## How It Works — Architecture

```
┌─────────────────────────────┐       UART        ┌─────────────────────────────┐
│         ESP2 (UI)           │ ◄────────────────► │        ESP1 (Worker)        │
│                             │  TX=32 ↔ RX=32     │                             │
│  • 0.96" OLED Display       │  TX=17 ↔ RX=17     │  • NRF24 #1 — HSPI          │
│  • Button UP   (GPIO25)     │                    │  • NRF24 #2 — VSPI          │
│  • Button DOWN (GPIO27)     │  Commands ───────► │  • WiFi Scanner             │
│  • Button MID  (GPIO26)     │  Results  ◄─────── │  • BLE Scanner              │
│  • NUKRAX glitch animation  │                    │  • JR.IO Disruptor          │
│  • Menu system              │                    │  • Alert Broadcasting        │
│  • Result display           │                    │  • Slide Switch (GPIO33)    │
└─────────────────────────────┘                    └─────────────────────────────┘
```

**ESP2** is the brain for UI — it receives button input, renders every screen, and sends commands over UART.

**ESP1** is the RF worker — it executes every scan, runs JR.IO, and broadcasts alerts. Results are sent back to ESP2 for display.

---

## Technical Specifications

| Parameter | Value |
|---|---|
| Microcontroller | ESP32-WROOM-32 (×2) |
| CPU | Xtensa LX6 Dual-Core 240 MHz |
| RF Modules | NRF24L01+PA/LNA (×2) |
| RF Frequency | 2.400 – 2.525 GHz |
| JR.IO TX Power | +20 dBm (PA/LNA amplified) |
| BLE TX Power (Alert) | +9 dBm (maximum) |
| WiFi TX Power (Alert) | 19.5 dBm (maximum) |
| NRF Scan Channels | 126 (0–125) |
| Display | SSD1306 128×64 OLED |
| Communication | UART 115200 baud |
| Power | 3.3V (via USB or external) |

---

## Support the Creator

If you found this project useful or just think it's cool, you can support the development with a crypto donation. Every contribution helps fund more open-source projects like this one.

<div align="center">

### ❤️ Support @CosmoLanex

**[👉 Donate via Crypto at cosmolanex.github.io/.cr/](https://cosmolanex.github.io/.cr/)**

*Accepts multiple cryptocurrencies — no account needed*

Your support keeps this project alive and helps build more open-source RF tools.

---

**If you can't donate, a ⭐ star on this repository means just as much.**

</div>

---

## License

```
MIT License — Copyright (c) 2026 CosmoLanex

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
```

---

<div align="center">

**SPECTER** · NUKRAX v3.0 · Developed by [@CosmoLanex](https://github.com/CosmoLanex)

*Built with ⚡ on ESP32*

</div>
