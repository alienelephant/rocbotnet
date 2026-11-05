#include "RDM6300Parser.h"

// Out-of-line definitions so the constants can be ODR-used (e.g. bound to a
// const& by std::max) without a link error. Required before C++17, and still
// required in C++17 for `static const` (only `constexpr` members became inline).
const uint32_t RDM6300Parser::DEFAULT_HOLDOFF_MS;
const size_t RDM6300Parser::DATA_LEN;
const uint8_t RDM6300Parser::STX;
const uint8_t RDM6300Parser::ETX;

static int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

RDM6300Parser::RDM6300Parser(uint32_t holdoffMs) : holdoffMs_(holdoffMs) {}

void RDM6300Parser::reset()
{
    len_ = 0;
    inFrame_ = false;
    haveCard_ = false;
}


bool RDM6300Parser::decode(uint64_t &id) const
{
    uint8_t bytes[6];
    for (size_t i = 0; i < 6; i++) {
        int hi = hexVal(buf_[i * 2]);
        int lo = hexVal(buf_[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    if ((bytes[0] ^ bytes[1] ^ bytes[2] ^ bytes[3] ^ bytes[4]) != bytes[5]) return false;

    id = 0;
    for (size_t i = 0; i < 5; i++) id = (id << 8) | bytes[i];
    return true;
}

bool RDM6300Parser::feed(uint8_t b, uint32_t nowMs)
{
    bytes_++;
    if (b == STX) {  // start of frame; also resyncs after a truncated one
        if (inFrame_) badFrames_++;
        inFrame_ = true;
        len_ = 0;
        return false;
    }
    if (!inFrame_) return false;  // noise between frames

    if (b != ETX) {
        if (len_ < DATA_LEN) {
            buf_[len_++] = (char)b;
        } else {  // overlong; wait for the next STX
            inFrame_ = false;
            badFrames_++;
        }
        return false;
    }

    inFrame_ = false;
    uint64_t id;
    if (len_ != DATA_LEN || !decode(id)) {
        badFrames_++;
        return false;
    }
    frames_++;

    bool isNew = !haveCard_ || id != lastCard_ || (uint32_t)(nowMs - lastSeenMs_) >= holdoffMs_;
    haveCard_ = true;
    lastCard_ = id;
    lastSeenMs_ = nowMs;
    return isNew;
}

void RDM6300Parser::toHex(uint64_t id, char out[11])
{
    static const char digits[] = "0123456789ABCDEF";
    for (int i = 9; i >= 0; i--) {
        out[i] = digits[id & 0xF];
        id >>= 4;
    }
    out[10] = '\0';
}

void RDM6300Parser::toBytes(uint64_t id, uint8_t out[5])
{
    for (int i = 4; i >= 0; i--) {
        out[i] = (uint8_t)id;
        id >>= 8;
    }
}
