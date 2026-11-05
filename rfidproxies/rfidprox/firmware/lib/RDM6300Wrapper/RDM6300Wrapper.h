// Arduino adapter for the RDM6300 reader: drains a Stream into RDM6300Parser.
//
#pragma once

#include <Arduino.h>
#include <RDM6300Parser.h>

class RDM6300Wrapper {
public:
    static const uint32_t BAUD = 9600;

    explicit RDM6300Wrapper(Stream &port, uint32_t holdoffMs = RDM6300Parser::DEFAULT_HOLDOFF_MS)
        : port_(port), parser_(holdoffMs) {}

    // Non-blocking; call it every loop(). Returns true once per card presentation.
    bool read();

    uint64_t lastCard() const { return parser_.lastCard(); }
    uint32_t lastCardNumber() const { return RDM6300Parser::cardNumber(parser_.lastCard()); }
    const char *lastCardHex() const { return hex_; }  // "1100773C55"; "" before the first card
    void lastCardBytes(uint8_t out[5]) const { RDM6300Parser::toBytes(parser_.lastCard(), out); }
    uint32_t bytes() const { return parser_.bytes(); }
    uint32_t frames() const { return parser_.frames(); }
    uint32_t badFrames() const { return parser_.badFrames(); }
    void reset() { parser_.reset(); }

private:
    Stream &port_;
    RDM6300Parser parser_;
    char hex_[11] = {0};
};
