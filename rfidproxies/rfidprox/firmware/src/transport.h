// The radio under rlink: moves sealed roclink frames to and from the gateway,
// and nothing else (security, ACKs and retries are rlink's). One is built in:
// Wi-Fi UDP (transport_udp.cpp), or LoRa when RFIDPROX_LORA is set
// (transport_lora.cpp).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace transport {

// Starts the radio. False (and logs why) if it can't.
bool begin();

// Keeps the link up (Wi-Fi rejoins); call often.
void loop();

// Whether frames can go out now (Wi-Fi joined; LoRa: the radio is up).
bool ready();

// Sends one frame; false if it didn't go out.
bool send(const uint8_t *frame, size_t len);

// A frame that has arrived, if any: its length, or 0. Never waits.
int receive(uint8_t *buf, size_t cap);

// For the log and the display: "Wi-Fi 10.42.0.53", "LoRa 915.0 MHz SF9".
const char *describe();

// How long to wait for the gateway's ACK to one frame, per attempt.
uint32_t ack_wait_ms();

}  // namespace transport
