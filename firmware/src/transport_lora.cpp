// LoRa to the rfgateway's SX1302 concentrator, on the Heltec V3/V4's SX1262.
// The radio settings are the gateway's (roclink-gateway group_vars: 915.0 MHz,
// SF9, 125 kHz, coding rate 4/7, preamble 8, the private sync word 0x12),
// which are RadioLib's defaults. The proxy is always-listening: between its own
// transmissions the radio stays in receive, so the gateway can reply after an
// uplink (150 ms after it ends) or send downlink at any time.
#if RFIDPROX_LORA
#include <Arduino.h>
#include <RadioLib.h>

#include "config.h"
#include "transport.h"

namespace transport {

static SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);
static bool radio_ok;
static volatile bool dio1;  // the radio raised DIO1: a packet arrived (or a transmission ended)
static float last_rssi, last_snr;

static void IRAM_ATTR on_dio1() { dio1 = true; }

static void listen() {
    dio1 = false;
    int st = radio.startReceive();
    if (st != RADIOLIB_ERR_NONE) Serial.printf("[lora] startReceive failed: %d\n", st);
}

bool begin() {
    int st = radio.begin(LORA_FREQ_MHZ, 125.0, 9, 7, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, LORA_POWER_DBM, 8,
                         LORA_TCXO_V, false);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("[lora] SX1262 didn't start: RadioLib error %d\n", st);
        return false;
    }
    radio.setPacketReceivedAction(on_dio1);
    radio_ok = true;
    listen();
    Serial.printf("[lora] SX1262 up: %.1f MHz, SF9, 125 kHz, CR 4/7, %d dBm, listening\n", LORA_FREQ_MHZ,
                  LORA_POWER_DBM);
    return true;
}

void loop() {}

bool ready() { return radio_ok; }

bool send(const uint8_t *frame, size_t len) {
    if (!radio_ok) return false;
#ifdef PIN_TX_LED
    digitalWrite(PIN_TX_LED, HIGH);
#endif
    int st = radio.transmit(frame, len);  // blocks for the airtime
#ifdef PIN_TX_LED
    digitalWrite(PIN_TX_LED, LOW);
#endif
    listen();  // straight back to receive: the gateway's reply comes 150 ms after this ends
    if (st != RADIOLIB_ERR_NONE) Serial.printf("[lora] transmit failed: %d\n", st);
    return st == RADIOLIB_ERR_NONE;
}

int receive(uint8_t *buf, size_t cap) {
    if (!dio1) return 0;
    size_t n = radio.getPacketLength();
    int st = n <= cap ? radio.readData(buf, n) : RADIOLIB_ERR_PACKET_TOO_LONG;
    last_rssi = radio.getRSSI();
    last_snr = radio.getSNR();
    listen();
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("[lora] dropped a %u-byte packet: RadioLib error %d (RSSI %.0f dBm)\n", (unsigned)n, st,
                      last_rssi);
        return 0;
    }
    Serial.printf("[lora] received %u bytes, RSSI %.0f dBm, SNR %.1f dB\n", (unsigned)n, last_rssi, last_snr);
    return (int)n;
}

const char *describe() {
    static char s[32];
    if (!radio_ok) return "LoRa: radio failed";
    if (last_rssi) snprintf(s, sizeof(s), "LoRa %.0f dBm %.0f dB", last_rssi, last_snr);
    else snprintf(s, sizeof(s), "LoRa %.1f MHz SF9", LORA_FREQ_MHZ);
    return s;
}

uint32_t ack_wait_ms() { return LORA_ACK_WAIT_MS; }

}  // namespace transport
#endif
