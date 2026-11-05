// rfidprox configuration. Override any of these in secrets.h (git-ignored),
// which is read first when it exists.
#pragma once

#if __has_include("secrets.h")
#include "secrets.h"
#endif

// --- Wi-Fi: the rfgateway's node access point (roclink-gateway, role node_ap)
#ifndef WIFI_SSID
#define WIFI_SSID "roclink-nodes"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "roclink-demo"
#endif
#ifndef GATEWAY_IP
#define GATEWAY_IP 10, 42, 0, 1  // roclink-gw on the Pi
#endif
#ifndef GATEWAY_PORT
#define GATEWAY_PORT 3333
#endif

// --- roclink identity (provisioning; see roclink-rfc tools/provision.py)
#ifndef ROCLINK_NET
#define ROCLINK_NET 0x42
#endif
#ifndef ROCLINK_NODE
#define ROCLINK_NODE 0x0301  // this proxy; the backend knows the box by it. One per
                             // board, never shared (docs/BOXLINK.md 9, "Node addresses")
#endif
#ifndef NODE_CHAIN_KEY
// The DEMO keys (provision.py --demo 0301 0302 0303 0304): anyone can derive them from
// the published test-vector master key. Put your own in secrets.h.
#if ROCLINK_NODE == 0x0301
#define NODE_CHAIN_KEY {0xE0, 0x41, 0xC3, 0xD7, 0x94, 0x25, 0x3B, 0x4B, \
                        0x6C, 0x50, 0x99, 0xE0, 0x6F, 0xA4, 0x9C, 0xE0}
#elif ROCLINK_NODE == 0x0302
#define NODE_CHAIN_KEY {0xEB, 0xF2, 0xFE, 0x1B, 0x4A, 0x95, 0x96, 0x25, \
                        0x96, 0x45, 0xDE, 0x23, 0xC7, 0x95, 0xD8, 0x6E}
#elif ROCLINK_NODE == 0x0303
#define NODE_CHAIN_KEY {0xA8, 0x70, 0x36, 0x6B, 0x75, 0x89, 0xCD, 0x73, \
                        0xF9, 0x85, 0xA1, 0xEA, 0xA5, 0x63, 0xF5, 0x42}
#elif ROCLINK_NODE == 0x0304
#define NODE_CHAIN_KEY {0x27, 0xC4, 0x7C, 0x07, 0x42, 0xB8, 0x03, 0xED, \
                        0x97, 0xAA, 0x42, 0x30, 0xA1, 0x0E, 0xBC, 0xCE}
#else
#error "no demo key for this ROCLINK_NODE: put NODE_CHAIN_KEY in secrets.h (provision.py)"
#endif
#define NODE_KEY_IS_DEMO 1
#endif

// --- timing (BACKEND.md section 4; first guesses)
#define HEARTBEAT_IDLE_MS (30UL * 1000UL)  // PING link check while idle (120 s in BACKEND.md; short for the bench)
#define ACK_WAIT_MS 700                    // per attempt, over Wi-Fi
#define RETRIES 2
#define AUTH_TIMEOUT_MS 10000              // no GRANT by then: "no answer"
#define SAME_CARD_MS 2000                  // a tag held on the reader counts once, until away this long
#define EPOCH_MS (24UL * 60UL * 60UL * 1000UL)  // roclink key ratchet: daily

// --- RDM6300 (125 kHz EM4100 reader, a UART; lib/RDM6300Wrapper). Diagnostics
// on the serial log: a summary every RDM6300_REPORT_MS (docs/BREADBOARD.md 6).
#ifndef RDM6300_DEBUG
#define RDM6300_DEBUG 1
#endif
#define RDM6300_REPORT_MS 5000UL

#define BOX_BAUD 115200

#if defined(RFIDPROX_BOARD_XIAO)
// --- pins: Seeed XIAO ESP32-S3 (docs/BOXLINK.md section 9). No display or
// buzzer: the user LED shows the state, and a push button logs in.
#define PIN_BUTTON 1        // D0: a push button to GND, the login button
#define PIN_LED 21          // the orange user LED, on when low
#define LED_ON LOW
#define PIN_BOX_TX 43       // D6: proxy TX -> box RX
#define PIN_BOX_RX 44       // D7: box TX -> proxy RX
#elif defined(RFIDPROX_BOARD_PICO2W)
// --- pins: Raspberry Pi Pico 2 W, the fob proxy (pico2w_fobproxy; hardware/pico2w:
// bom.yaml, WIRING.md and a Velxio layout). GPIO numbers; the header pin is in
// each comment. No display, button or buzzer: a fob tapped on the RDM6300 logs
// in, and the onboard LED (the core's PIN_LED, on the Wi-Fi chip) shows the state.
#define PIN_RDM6300_RX 5    // pin 7, UART1 RX: the RDM6300's TX through a 10k/20k divider
#define PIN_RDM6300_TX 4    // pin 6, UART1 TX: not connected (the reader is told nothing)
#define LED_ON HIGH
#define PIN_BOX_TX 0        // pin 1, UART0 TX: proxy TX -> box RX (docs/BOXLINK.md)
#define PIN_BOX_RX 1        // pin 2, UART0 RX: box TX -> proxy RX
#ifndef LOGIN_FOB
#define LOGIN_FOB 1         // logins come from the fob reader, not a button
#endif
#else
// --- pins: Heltec WiFi LoRa 32 V3 or V4 (docs/BREADBOARD.md). GPIO numbers;
// the header pin is in each comment. Chosen from the GPIOs Heltec recommends
// for add-ons that neither board uses: on the V4, GPIO 2 and 7 drive the LoRa
// front end and 38-42 the GNSS connector.
#define HAS_OLED 1
#define PIN_RDM6300_RX 4   // J3-15: the RDM6300's TX, through a 5 V to 3.3 V divider
#define PIN_BUZZER 3      // J3-14 (optional)
#define PIN_LED 35         // on board (J2-10)
#define LED_ON HIGH
#define PIN_TX_LED PIN_LED // flashes on each transmission
#define PIN_BUTTON 0       // on board: PRG (the game-box build's login)
#define PIN_BOX_TX 40      // J3-9: proxy TX -> box RX (docs/BOXLINK.md)
#define PIN_BOX_RX 39      // J3-10: box TX -> proxy RX
#define PIN_VEXT 36        // on board (J2-9): powers the OLED and the Ve pins
#if defined(RFIDPROX_BOARD_V4)
#define VEXT_ON HIGH       // V4 datasheet 3.3: VextCtrl high
#else
#define VEXT_ON LOW        // V3: low
#endif
#define PIN_OLED_SDA 17    // on board
#define PIN_OLED_SCL 18    // on board
#define PIN_OLED_RST 21    // on board (J2-16)
// The SX1262, on board (its SPI is the default bus: SCK 9, MISO 11, MOSI 10)
#define PIN_LORA_NSS 8
#define PIN_LORA_DIO1 14
#define PIN_LORA_RST 12
#define PIN_LORA_BUSY 13
#define LORA_TCXO_V 1.8    // the module's TCXO, powered from DIO3
#endif

// --- LoRa (RFIDPROX_LORA): must match the gateway (roclink-gateway group_vars)
#define LORA_FREQ_MHZ 915.0
#define LORA_POWER_DBM 14
// An ACK comes 150 ms after the uplink ends and takes ~200 ms of airtime at SF9.
#define LORA_ACK_WAIT_MS 1200

// --- the game-box build (gamebox.cpp): the dummy fob the login button uses
#ifndef DUMMY_FOB
#define DUMMY_FOB {0xF0, 0xB0, (uint8_t)(ROCLINK_NODE >> 8), (uint8_t)ROCLINK_NODE}  // f0b00301
#endif
// 1: no button needed: it logs in with the dummy fob by itself once its link
// is up, and again until the game says go (xiao_gamebox_auto)
#ifndef AUTO_LOGIN
#define AUTO_LOGIN 0
#endif

// 1: the game-box build logs in with a tapped fob (RDM6300) instead of a button
#ifndef LOGIN_FOB
#define LOGIN_FOB 0
#endif

#define FW_VERSION 0x0001
