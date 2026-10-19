# rfidprox: hardware

| | |
|---|---|
| Status | Draft. Pin assignments to be checked against the board before wiring (§2.3). |

## 1. Choosing a board

The proxy needs a radio, an RC522 on SPI, a UART to the box, and some way to show the user what happened. Counting pins:

| Part | Pins |
|---|---|
| SX1262 | SPI (SCK, MOSI, MISO) + CS, RST, BUSY, DIO1 |
| RC522 | SPI (shared or its own) + CS, RST (IRQ optional) |
| Box UART | TX, RX |
| Feedback | LED, buzzer, display |

That's 12–13 GPIOs even with the two SPI devices on one bus. The XIAO ESP32-S3 has 11 (D0–D10).

| Board | Radio | Free GPIOs | For | Against |
|---|---|---|---|---|
| **Heltec WiFi LoRa 32 V3** (chosen) | SX1262 on board | about 15 | Radio and OLED already wired; same family as the V2 boards in use | Larger |
| XIAO ESP32-S3 + Wio-SX1262 kit | SX1262 on the board-to-board connector | D0–D7 after the radio | Tiny | No spare pins; no display |
| ESP32-S3-DevKitC-1 + SX1262 breakout | separate module | 30+ | Most pins, easy to probe | Loose wiring: a bench board only |
| A HaLow board (ESP32-S3 + Morse Micro MM6108) | Wi-Fi HaLow | depends | For the HaLow transport later | Check availability; needs a HaLow access point |

## 2. Heltec WiFi LoRa 32 V3

### 2.1 Fixed on the board

From the board's documentation and community pin maps (not changeable):

| Function | GPIO |
|---|---|
| SX1262 NSS / SCK / MOSI / MISO | 8 / 9 / 10 / 11 |
| SX1262 RST / BUSY / DIO1 | 12 / 13 / 14 |
| OLED SDA / SCL / RST | 17 / 18 / 21 |
| Vext power switch (low = on: powers the OLED and the header's Vext pin) | 36 |
| White LED | 35 |
| PRG button | 0 |
| Battery voltage (ADC) | 1 |
| USB serial (CP2102) | 43 / 44 |

### 2.2 Proposed wiring

The RC522 gets its own SPI bus (the ESP32-S3's second general-purpose SPI controller), so it doesn't share the radio's bus or timing.

| Part | Signal | GPIO | Notes |
|---|---|---|---|
| RC522 | SCK | 5 | second SPI bus |
| | MOSI | 6 | |
| | MISO | 7 | |
| | SDA (chip select) | 4 | |
| | RST | 3 | |
| | 3.3 V, GND | 3V3, GND | the RC522 is a 3.3 V part: never 5 V |
| Box UART (UART1) | proxy TX → box RX | 47 | |
| | box TX → proxy RX | 48 | |
| Buzzer | | 2 | passive piezo via a small transistor, or an active buzzer |
| Status | | 35 | on-board LED |
| OLED | | 17 / 18 / 21, Vext 36 | on board |

Avoid the strapping pins (0, 3, 45, 46) for anything that could hold a level at reset. GPIO3 is a strapping pin, but only for JTAG source selection, so it's safe as the RC522's reset. GPIO 33–37 are tied to the flash and PSRAM on some ESP32-S3 variants.

### 2.3 To verify before wiring

- That GPIO 47 and 48 are on the V3's header. If not, use 41 / 42 for the box UART.
- That GPIO 2–7 are free on the header with the OLED in use.
- The SX1262's TCXO voltage (1.8 V on the V3) and DIO2 RF-switch setting, for RadioLib.

## 3. Power

| Part | Typical | Peak |
|---|---|---|
| ESP32-S3, Wi-Fi on | 80 mA | 350 mA |
| SX1262, 14 dBm transmit | | 45 mA (about 120 mA at 21 dBm) |
| RC522, field on | 13–26 mA | |
| OLED | 10–20 mA | |

A 5 V, 1 A USB-C supply covers it. The proxy is mains-powered by design (an always-listening node). The box has its own supply (BOXLINK.md §1).

## 4. Bill of materials (one proxy)

| Item | Qty | Notes |
|---|---|---|
| Heltec WiFi LoRa 32 V3, 863–928 MHz | 1 | with its antenna |
| RC522 module (13.56 MHz) | 1 | the common blue board; reads MIFARE Classic, Ultralight, NTAG |
| ISO 14443A fobs or cards | per user | MIFARE Classic 1K or NTAG215. 125 kHz fobs don't work. |
| Buzzer | 1 | |
| JST-XH 3-pin socket and lead | 1 | to the box (BOXLINK.md §1) |
| USB-C 5 V 1 A supply | 1 | |
| Enclosure | 1 | the RC522's antenna behind at most a few mm of plastic, away from metal |

## 5. The RC522 and future cards

The RC522 handles ISO 14443A up to MIFARE Classic authentication and Ultralight C. For challenge-response cards later (NTAG424 DNA, DESFire EV2/EV3, DESIGN.md §7.4), its ISO 14443-4 support in Arduino libraries is limited. A PN532 is the likely replacement then: same bus, more pins (IRQ).
