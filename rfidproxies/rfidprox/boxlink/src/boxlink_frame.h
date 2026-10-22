// boxlink frames (docs/BOXLINK.md sections 2-5): the bytes on the wire between
// a box and its rfidprox. No I/O and no Arduino here, so both ends (and host
// tests) share it.
//
//   on the wire:  COBS( ver_type | flags | seq | schema | payload | crc16 )  0x00
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace boxlink {

constexpr uint8_t VERSION = 1;
constexpr size_t MAX_PAYLOAD = 200;
constexpr size_t MAX_FRAME = 4 + MAX_PAYLOAD + 2;          // header, payload, CRC
constexpr size_t MAX_WIRE = MAX_FRAME + MAX_FRAME / 254 + 2;  // COBS overhead and the delimiter

// Frame types (section 3)
constexpr uint8_t DATA = 0;  // box data: schemas 32-255, bytes the proxy never looks inside
constexpr uint8_t ACK = 1;   // acknowledges seq; a 1-byte payload is a refusal
constexpr uint8_t CTRL = 2;  // control operations, the op in `schema`

constexpr uint8_t FLAG_ACK_REQ = 0x01;
constexpr uint8_t FIRST_BOX_SCHEMA = 32;

// Control operations (section 4)
constexpr uint8_t OP_HELLO = 1;           // box -> proxy: ver, box_type (length-prefixed), box_version u16
constexpr uint8_t OP_HELLO_ACK = 2;       // proxy -> box: ver, proxy_version u16, max_payload, link, session u16
constexpr uint8_t OP_PING = 3;            // box -> proxy: answered with STATUS
constexpr uint8_t OP_STATUS = 4;          // proxy -> box: link, session u16, budget
constexpr uint8_t OP_SESSION_OPEN = 5;    // proxy -> box: session u16, lifetime_s u16, label (length-prefixed)
constexpr uint8_t OP_SESSION_CLOSED = 6;  // proxy -> box: session u16, reason
constexpr uint8_t OP_END_SESSION = 7;     // box -> proxy: session u16, reason
constexpr uint8_t OP_DENIED = 8;          // proxy -> box: result, verdict, arg u16, label
constexpr uint8_t OP_TRIGGER = 9;         // box -> proxy: trigger, arg (section 9, the demo)
constexpr uint8_t OP_VERDICT = 10;        // proxy -> box: result, arg u16, solved, required (section 9)

// Triggers (OP_TRIGGER, section 9)
constexpr uint8_t TRIGGER_WIN = 1;
constexpr uint8_t TRIGGER_FAIL = 2;
constexpr uint8_t TRIGGER_HINT = 3;  // arg: the hint number, 0 for the next one
constexpr uint8_t TRIGGER_END = 4;

// Link states (section 4.1)
constexpr uint8_t LINK_DOWN = 0;
constexpr uint8_t LINK_UP = 1;
constexpr uint8_t LINK_GRACE = 2;

// Refusals: an ACK's 1-byte payload (section 5)
constexpr uint8_t REFUSED_NO_SESSION = 1;
constexpr uint8_t REFUSED_BUSY = 2;
constexpr uint8_t REFUSED_TOO_LONG = 3;
constexpr uint8_t REFUSED_BAD_SCHEMA = 4;
constexpr uint8_t REFUSED_STALE = 5;
constexpr uint8_t REFUSED_OFFLINE = 6;
constexpr uint8_t REFUSED_UNKNOWN = 7;

struct Frame {
    uint8_t type;
    uint8_t flags;
    uint8_t seq;
    uint8_t schema;  // DATA: the box's schema; CTRL: the op; ACK: 0
    uint8_t length;  // of payload
    uint8_t payload[MAX_PAYLOAD];
};

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF).
uint16_t crc16(const uint8_t *data, size_t len);

// The frame as it goes on the wire, ending with the 0x00 delimiter. `out` holds
// at least MAX_WIRE bytes. Returns the length, or 0 if the payload is too long.
size_t encode(const Frame &f, uint8_t *out);

// Collects bytes from the wire into frames. Bad frames (COBS, length, CRC,
// version) are dropped and counted: the sender's retry covers them.
struct Decoder {
    uint8_t buf[MAX_WIRE];
    size_t len;
    bool overflow;
    uint32_t dropped;

    // Takes one byte; true when it completed a good frame, now in `out`.
    bool feed(uint8_t byte, Frame *out);
};

}  // namespace boxlink
