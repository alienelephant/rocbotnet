/*
  roclink over LoRa: sender

  RadioLib's "SX127x_Transmit_Blocking" example, set up for a Heltec WiFi
  LoRa 32 V2 at 915 MHz, with each packet wrapped in a roclink envelope.
  Lines marked "// roclink" are the roclink changes; the pins and frequency
  are the board changes. Everything else is the stock example.

  Pair it with lora_receiver. Both ends must use the same frequency,
  bandwidth, spreading factor, coding rate, sync word and preamble length
  (the RadioLib defaults are used for everything except the frequency).
*/

// include the library
#include <RadioLib.h>

#include <roclink.h>  // roclink

#define ROCLINK_NET 0xF0     // roclink: an open test net: unsecured, anyone may join (PROTOCOL.md Annex B)
#define ROCLINK_NODE 0x1234  // roclink: this node's address; give each node its own
#define SCHEMA_HELLO 1         // roclink: what the payload is (your choice, 1..255)

// Heltec WiFi LoRa 32 V2 (SX1276) has the following connections:
// NSS pin:   18
// DIO0 pin:  26
// RESET pin: 14
// DIO1 pin:  35
SX1276 radio = new Module(18, 26, 14, 35);

roclink_client_t node;  // roclink

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

  roclink_client_config_t cfg = {ROCLINK_NET, ROCLINK_NODE, ROCLINK_ADDR_ANY_GATEWAY};  // roclink
  roclink_client_init(&node, &cfg, (uint8_t)esp_random());                              // roclink
}

// counter to keep track of transmitted packets
int count = 0;

void loop() {
  Serial.print(F("[SX1276] Transmitting packet ... "));

  String str = "Hello World! #" + String(count++);

  // roclink: wrap the message in an envelope (9-byte header + your bytes)
  uint8_t frame[ROCLINK_FRAME_MAX_LEN];
  int len = roclink_client_build_data(&node, SCHEMA_HELLO, (const uint8_t *)str.c_str(), str.length(),
                                      ROCLINK_SEND_DEFAULT, frame, sizeof(frame));
  if (len < 0) {                                              // roclink: no frame was built,
    Serial.printf("roclink: %s\n", roclink_status_str(len));  // roclink: so send nothing
    delay(1000);
    return;
  }

  int state = radio.transmit(frame, len);  // roclink: send the frame instead of the string

  if (state == RADIOLIB_ERR_NONE) {
    // the packet was successfully transmitted
    Serial.print(F(" success! "));
    Serial.print(len);
    Serial.println(F("-byte roclink frame"));

  } else if (state == RADIOLIB_ERR_PACKET_TOO_LONG) {
    // the supplied packet was longer than 256 bytes
    Serial.println(F("too long!"));

  } else if (state == RADIOLIB_ERR_TX_TIMEOUT) {
    // timeout occurred while transmitting packet
    Serial.println(F("timeout!"));

  } else {
    // some other error occurred
    Serial.print(F("failed, code "));
    Serial.println(state);

  }

  // wait for a second before transmitting again
  delay(1000);
}
