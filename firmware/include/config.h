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
#define ROCLINK_NODE 0x0301  // this proxy; the backend knows the box by it
#endif
#ifndef NODE_CHAIN_KEY
// The DEMO key for node 0x0301 (provision.py --demo 0301): anyone can derive it
// from the published test-vector master key. Put your own in secrets.h.
#define NODE_CHAIN_KEY {0xE0, 0x41, 0xC3, 0xD7, 0x94, 0x25, 0x3B, 0x4B, \
                        0x6C, 0x50, 0x99, 0xE0, 0x6F, 0xA4, 0x9C, 0xE0}
#define NODE_KEY_IS_DEMO 1
#endif

// --- timing (BACKEND.md section 4; first guesses)
#define HEARTBEAT_IDLE_MS (30UL * 1000UL)  // PING link check while idle (120 s in BACKEND.md; short for the bench)
#define ACK_WAIT_MS 700                    // per attempt, over Wi-Fi
#define RETRIES 2
#define AUTH_TIMEOUT_MS 10000              // no GRANT by then: "no answer"
#define SAME_CARD_MS 2000                  // a card held on the reader counts once
#define EPOCH_MS (24UL * 60UL * 60UL * 1000UL)  // roclink key ratchet: daily

// --- pins: Heltec WiFi LoRa 32 V3 (docs/BREADBOARD.md)
#define PIN_RC522_SCK 5
#define PIN_RC522_MOSI 6
#define PIN_RC522_MISO 7
#define PIN_RC522_SS 4
#define PIN_RC522_RST 3
#define PIN_BUZZER 2
#define PIN_LED 35        // on board
#define PIN_VEXT 36       // on board: LOW powers the OLED
#define PIN_OLED_SDA 17   // on board
#define PIN_OLED_SCL 18   // on board
#define PIN_OLED_RST 21   // on board

#define FW_VERSION 0x0001
