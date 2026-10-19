// The proxy's roclink link: a secured, always-listening roclink node on a
// transport. This first version's transport is Wi-Fi UDP to the rfgateway
// (DESIGN.md section 6.4); LoRa comes behind the same interface later.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace rlink {

// Called for each new downlink DATA frame (not duplicates, not control).
using DownlinkHandler = void (*)(uint8_t schema, const uint8_t *payload, size_t len);

// Restores the key and frame counter from NVS, sets up security, starts Wi-Fi.
// Returns false (and logs why) if security can't start.
bool begin(DownlinkHandler on_downlink);

// Keeps Wi-Fi up, ratchets the key daily, and handles any frame that arrived.
void loop();

bool wifi_up();

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
