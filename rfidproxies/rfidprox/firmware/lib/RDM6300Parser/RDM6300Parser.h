// Framework-agnostic parser for the RDM6300 125 kHz RFID reader.
//
// Frame format (14 bytes):
//   0x02 | 10 ASCII hex chars (5-byte ID) | 2 ASCII hex chars (checksum) | 0x03
// The checksum is the XOR of the 5 ID bytes.
//
// The reader repeats the frame for as long as a card stays near the reader.
// feed() turns that stream into one event per presentation: a card counts as
// "new" if it differs from the last one, or if no valid frame for it has
// arrived for holdoffMs.
// this looks like a much better library: https://github.com/Sthing/AccessThing/tree/master/Arduino/libraries/EM4100
#pragma once

#include <stddef.h>
#include <stdint.h>

class RDM6300Parser {
public:
    static const uint32_t DEFAULT_HOLDOFF_MS = 500;

    explicit RDM6300Parser(uint32_t holdoffMs = DEFAULT_HOLDOFF_MS);

    // Feed one byte received at time nowMs (any monotonic ms counter; wrap-around
    // is fine). Returns true when this byte completed a valid frame for a newly
    // presented card; read it with lastCard().
    bool feed(uint8_t b, uint32_t nowMs);

    // Full 40-bit ID of the most recent card (0 if none yet).
    uint64_t lastCard() const { return lastCard_; }

    // Running counters, for diagnostics (they wrap at 2^32). Take differences
    // between two readings for a rate.
    uint32_t bytes() const { return bytes_; }          // every byte fed
    uint32_t frames() const { return frames_; }        // valid frames, repeats included
    uint32_t badFrames() const { return badFrames_; }  // bad length, hex or checksum

    // Forget the last card (the next frame always counts as new) and drop any
    // partial frame.
    void reset();

    // The low 32 bits: usually the number printed on the card or fob.
    static uint32_t cardNumber(uint64_t id) { return (uint32_t)(id & 0xFFFFFFFFu); }

    // Upper-case 10-digit hex, e.g. "1100773C55". out must hold 11 chars.
    // Portable alternative to printf("%llX"), which some platforms lack (AVR).
    static void toHex(uint64_t id, char out[11]);

    // The 5 ID bytes as the card sends them, big-endian (0x1100773C55 -> 11 00 77 3C 55).
    static void toBytes(uint64_t id, uint8_t out[5]);

private:
    static const size_t DATA_LEN = 12;  // 10 ID chars + 2 checksum chars
    static const uint8_t STX = 0x02;
    static const uint8_t ETX = 0x03;

    bool decode(uint64_t &id) const;

    uint32_t holdoffMs_;
    char buf_[DATA_LEN];
    size_t len_ = 0;
    bool inFrame_ = false;

    uint64_t lastCard_ = 0;
    bool haveCard_ = false;
    uint32_t lastSeenMs_ = 0;
    uint32_t bytes_ = 0;
    uint32_t frames_ = 0;
    uint32_t badFrames_ = 0;
};
