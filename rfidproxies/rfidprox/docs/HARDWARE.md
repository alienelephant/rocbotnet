# rfidprox: hardware

| | |
|---|---|
| Status | Draft. Pin assignments to be checked against the board before wiring (§2.3). |

## 1. Choosing a board

The proxy needs a radio, an RDM6300 on a UART, a UART to the box, and some way to show the user what happened. Counting pins:

| Part | Pins |
|---|---|
| SX1262 | SPI (SCK, MOSI, MISO) + CS, RST, BUSY, DIO1 |
| RDM6300 | one UART input (its TX) |
| Box UART | TX, RX |
| Feedback | LED, buzzer, display |

That's 8 GPIOs beside the radio's 7. The XIAO ESP32-S3 has 11 (D0–D10).

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
| RDM6300 TX (through a 5 V to 3.3 V divider) | 4 | J3-15 |
| RDM6300 +5V, GND | | J2-2, J2-1 |
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
| RDM6300 (5 V) | about 50 mA | |
| OLED | 10–20 mA | |

A 5 V, 1 A USB-C supply covers it. The proxy is mains-powered by design (an always-listening node). The box has its own supply (BOXLINK.md §1).

## 4. Bill of materials (one proxy)

| Item | Qty | Notes |
|---|---|---|
| Heltec WiFi LoRa 32 V3, 863–928 MHz | 1 | with its antenna |
| RDM6300 module (125 kHz), with its coil | 1 | reads EM4100 tags; 5 V, UART at 9600 baud |
| 10 kΩ and 20 kΩ resistors | 1 each | the divider on its TX line (BREADBOARD.md §2.2) |
| EM4100 fobs or cards | per user | 125 kHz. 13.56 MHz cards don't work. |
| Buzzer | 1 | |
| JST-XH 3-pin socket and lead | 1 | to the box (BOXLINK.md §1) |
| USB-C 5 V 1 A supply | 1 | |
| Enclosure | 1 | the RDM6300's coil behind at most a few mm of plastic, away from metal |

## 5. The RDM6300 and future cards

The RDM6300 reads EM4100 tags: a fixed 5-byte ID with no security, easily copied. For challenge-response cards later (NTAG424 DNA, DESFire EV2/EV3, DESIGN.md §7.4), the proxy needs a 13.56 MHz reader instead; a PN532 is the likely one (SPI or I2C, plus IRQ).
