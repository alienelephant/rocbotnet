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
| **Heltec WiFi LoRa 32 V3 or V4** (chosen; the V4 is pin-compatible) | SX1262 on board | about 10 safe ones | Radio and OLED already wired; same family as the V2 boards in use | Larger |
| XIAO ESP32-S3 + Wio-SX1262 kit | SX1262 on the board-to-board connector | D0–D7 after the radio | Tiny | No spare pins; no display |
| ESP32-S3-DevKitC-1 + SX1262 breakout | separate module | 30+ | Most pins, easy to probe | Loose wiring: a bench board only |
| A HaLow board (ESP32-S3 + Morse Micro MM6108) | Wi-Fi HaLow | depends | For the HaLow transport later | Check availability; needs a HaLow access point |

## 2. Heltec WiFi LoRa 32 V3 and V4

### 2.1 Fixed on the board

From the board's documentation and community pin maps (not changeable):

| Function | GPIO |
|---|---|
| SX1262 NSS / SCK / MOSI / MISO | 8 / 9 / 10 / 11 |
| SX1262 RST / BUSY / DIO1 | 12 / 13 / 14 |
| OLED SDA / SCL / RST | 17 / 18 / 21 |
| Vext power switch (powers the OLED and the Ve pins; V3: low = on, V4: high = on) | 36 |
| White LED | 35 |
| PRG button | 0 |
| Battery voltage (ADC) | 1 |
| USB serial | V3: a CP2102 on 43 / 44; V4: native USB on 19 / 20 |
| V4 only: LoRa power amplifier (FEM) | 2, 7, 46 |
| V4 only: GNSS connector | 34 (power), 38–42 |

### 2.2 Wiring

The pin assignments, with header pins and the full J2/J3 tables, are in [BREADBOARD.md](BREADBOARD.md) §2. In short:

| Part | GPIO | Header pin |
|---|---|---|
| RC522 SDA, RST | 47, 48 | J2-13, J2-14 |
| RC522 MISO, SCK, MOSI | 4, 5, 6 | J3-15, J3-16, J3-17 |
| Buzzer | 3 | J3-14 |
| Box UART TX, RX (later) | 39, 40 | J3-10, J3-9 |

These work on both the **V3** and the **V4**, which is pin-compatible with it. On the V4, GPIO 2 and 7 control the LoRa power amplifier and 38–42 the GNSS connector, so they're avoided; the box UART on 39/40 assumes no GNSS module is fitted.

### 2.3 Still to check

- The SX1262's TCXO voltage and DIO2 RF-switch setting for RadioLib, and on a V4 its front-end pins (GPIO 2, 7, 46), when LoRa is added.

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
