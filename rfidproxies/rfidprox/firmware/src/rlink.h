// The proxy's roclink link: a secured, always-listening roclink node on a
// transport (transport.h): Wi-Fi UDP to the rfgateway, or LoRa with
// RFIDPROX_LORA (DESIGN.md section 6.4).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace rlink {

// Called for each new downlink DATA frame (not duplicates, not control).
using DownlinkHandler = void (*)(uint8_t schema, const uint8_t *payload, size_t len);

// Restores the key and frame counter from NVS, sets up security, starts the radio.
// Returns false (and logs why) if security can't start.
bool begin(DownlinkHandler on_downlink);

// Keeps the radio's link up, ratchets the key daily, and handles any frame that arrived.
void loop();

// Whether frames can go out (Wi-Fi joined; LoRa: the radio is up).
bool ready();

// The link, for the log and the display ("Wi-Fi 10.42.0.53", "LoRa -62 dBm 9 dB").
const char *describe();

// Sends one DATA frame. With `ack`, waits for the gateway's ACK and retries the
// identical bytes; returns whether it was ACKed. Without, returns whether it
// went out. Frames that arrive meanwhile are handled as in loop().
bool send(uint8_t schema, const uint8_t *payload, size_t len, bool ack);

// A PING link check (PROTOCOL.md 7.1): true if the gateway ACKed it.
bool ping();

// For the display and the log.
struct Stats {
    uint32_t sent, acked, unacked, downlinks, refused;
    uint32_t last_ack_ms;  // millis() of the last ACK, 0 if none yet
    uint32_t fcnt;         // the next frame counter
};
const Stats &stats();

}  // namespace rlink
