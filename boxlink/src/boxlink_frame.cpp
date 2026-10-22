#include "boxlink_frame.h"

#include <string.h>

namespace boxlink {

uint16_t crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// COBS: every 0x00 is replaced by the distance to the next one, so the encoded
// bytes contain no zeros and 0x00 can end the frame.
static size_t cobs_encode(const uint8_t *in, size_t len, uint8_t *out) {
    size_t code_at = 0, o = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < len; ++i) {
        if (in[i] == 0) {
            out[code_at] = code;
            code_at = o++;
            code = 1;
            continue;
        }
        out[o++] = in[i];
        if (++code == 0xFF) {
            out[code_at] = code;
            code_at = o++;
            code = 1;
        }
    }
    out[code_at] = code;
    return o;
}

// Returns the decoded length, or 0 if it isn't valid COBS.
static size_t cobs_decode(const uint8_t *in, size_t len, uint8_t *out) {
    size_t i = 0, o = 0;
    while (i < len) {
        uint8_t code = in[i++];
        if (code == 0 || i + code - 1 > len) return 0;
        for (uint8_t k = 1; k < code; ++k) {
            if (in[i] == 0) return 0;
            out[o++] = in[i++];
        }
        if (code != 0xFF && i < len) out[o++] = 0;
    }
    return o;
}

size_t encode(const Frame &f, uint8_t *out) {
    if (f.length > MAX_PAYLOAD) return 0;
    uint8_t raw[MAX_FRAME];
    raw[0] = (uint8_t)(VERSION << 5 | (f.type & 0x07) << 2);
    raw[1] = f.flags;
    raw[2] = f.seq;
    raw[3] = f.schema;
    memcpy(raw + 4, f.payload, f.length);
    size_t n = 4 + f.length;
    uint16_t crc = crc16(raw, n);
    raw[n++] = (uint8_t)crc;
    raw[n++] = (uint8_t)(crc >> 8);
    size_t w = cobs_encode(raw, n, out);
    out[w++] = 0x00;
    return w;
}

bool Decoder::feed(uint8_t byte, Frame *out) {
    if (byte != 0x00) {
        if (len < sizeof(buf)) buf[len++] = byte;
        else overflow = true;
        return false;
    }
    size_t got = len;
    bool too_long = overflow;
    len = 0;
    overflow = false;
    if (got == 0) return false;  // an empty frame: a delimiter to resynchronise
    uint8_t raw[MAX_WIRE];
    size_t n = too_long ? 0 : cobs_decode(buf, got, raw);
    if (n < 6 || n > MAX_FRAME || raw[0] >> 5 != VERSION || (raw[0] & 0x03) ||
        crc16(raw, n - 2) != (uint16_t)(raw[n - 2] | raw[n - 1] << 8)) {
        dropped++;
        return false;
    }
    out->type = (raw[0] >> 2) & 0x07;
    out->flags = raw[1];
    out->seq = raw[2];
    out->schema = raw[3];
    out->length = (uint8_t)(n - 6);
    memcpy(out->payload, raw + 4, out->length);
    return true;
}

}  // namespace boxlink
