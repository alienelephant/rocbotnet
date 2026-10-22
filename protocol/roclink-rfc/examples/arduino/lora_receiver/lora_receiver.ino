/*
  roclink over LoRa: receiver

  RadioLib's "SX127x_Receive_Blocking" example, set up for a Heltec WiFi
  LoRa 32 V2 at 915 MHz, unwrapping roclink envelopes. Lines marked
  "// roclink" are the roclink changes; the pins and frequency are the board
  changes. Everything else is the stock example.
*/

// include the library
#include <RadioLib.h>

#include <roclink.h>  // roclink

#define ROCLINK_NET 0xF0        // roclink: an open test net: unsecured, anyone may join (PROTOCOL.md Annex B)
#define ROCLINK_GATEWAY 0xFF01  // roclink: this gateway's address (0xFF00..0xFFFE)

// Heltec WiFi LoRa 32 V2 (SX1276) has the following connections:
// NSS pin:   18
// DIO0 pin:  26
// RESET pin: 14
// DIO1 pin:  35
SX1276 radio = new Module(18, 26, 14, 35);

roclink_server_t gateway;  // roclink
roclink_peer_t peers[16];  // roclink: per-node state (duplicates), up to 16 nodes

void setup() {
  Serial.begin(115200);

  // initialize SX1276 at 915 MHz
  Serial.print(F("[SX1276] Initializing ... "));
  ConfigLoRa_t config;
  config.frequency = 915;
  int state = radio.begin(config);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println(F("success!"));
  } else {
    Serial.print(F("failed, code "));
    Serial.println(state);
    while (true) { delay(10); }
  }

  // roclink: set up the gateway side
  roclink_server_config_t cfg;
  roclink_server_config_default(&cfg);
  cfg.net_id = ROCLINK_NET;
  cfg.gateway_id = ROCLINK_GATEWAY;
  roclink_server_init(&gateway, &cfg, peers, 16, esp_random());
}

void loop() {
  Serial.print(F("[SX1276] Waiting for incoming transmission ... "));

  // receive the packet as a byte array (a roclink frame is binary)
  uint8_t frame[256];
  int state = radio.receive(frame, 0);  // 0 = any length

  if (state == RADIOLIB_ERR_NONE) {
    // packet was successfully received
    Serial.println(F("success!"));

    // roclink: open the envelope
    roclink_rx_t rx;
    roclink_server_handle(&gateway, frame, radio.getPacketLength(), millis(), &rx);
    if (rx.kind == ROCLINK_RX_DATA) {
      Serial.printf("[roclink] node %04X, schema %u, seq %u\n", rx.hdr.src, rx.hdr.schema, rx.hdr.seq);
      Serial.printf("[roclink] Data:\t\t\t%.*s\n", (int)rx.payload_len, (const char *)rx.payload);
    } else if (rx.kind == ROCLINK_RX_DUPLICATE) {
      Serial.println(F("[roclink] duplicate, ignored"));
    } else {
      Serial.printf("[roclink] not for us (%s)\n", roclink_status_str(rx.status));
    }

    // print the RSSI (Received Signal Strength Indicator)
    // of the last received packet
    Serial.print(F("[SX1276] RSSI:\t\t\t"));
    Serial.print(radio.getRSSI());
    Serial.println(F(" dBm"));

    // print the SNR (Signal-to-Noise Ratio)
    // of the last received packet
    Serial.print(F("[SX1276] SNR:\t\t\t"));
    Serial.print(radio.getSNR());
    Serial.println(F(" dB"));

  } else if (state == RADIOLIB_ERR_RX_TIMEOUT) {
    // timeout occurred while waiting for a packet
    Serial.println(F("timeout!"));

  } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
    // packet was received, but is malformed
    Serial.println(F("CRC error!"));

  } else {
    // some other error occurred
    Serial.print(F("failed, code "));
    Serial.println(state);

  }
}
