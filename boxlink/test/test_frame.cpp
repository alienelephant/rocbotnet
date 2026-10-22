// Host tests for boxlink framing:
//   g++ -std=c++17 -Wall -Wextra -Isrc test/test_frame.cpp src/boxlink_frame.cpp -o /tmp/t && /tmp/t
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boxlink_frame.h"

using namespace boxlink;

static int fails;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            fails++;                                                   \
        }                                                              \
    } while (0)

static bool same(const Frame &a, const Frame &b) {
    return a.type == b.type && a.flags == b.flags && a.seq == b.seq && a.schema == b.schema &&
           a.length == b.length && !memcmp(a.payload, b.payload, a.length);
}

// Feeds the wire bytes; returns how many frames came out (the last in *out).
static int feed(Decoder &d, const uint8_t *w, size_t n, Frame *out) {
    int frames = 0;
    for (size_t i = 0; i < n; ++i)
        if (d.feed(w[i], out)) frames++;
    return frames;
}

int main() {
    // The CRC's check value (CRC-16/CCITT-FALSE of "123456789").
    CHECK(crc16((const uint8_t *)"123456789", 9) == 0x29B1);

    // Every payload length, with zeros and 0xFF runs: the COBS edge cases.
    srand(1);
    for (int len = 0; len <= (int)MAX_PAYLOAD; ++len) {
        Frame f = {DATA, FLAG_ACK_REQ, (uint8_t)len, 40, (uint8_t)len, {}};
        for (int i = 0; i < len; ++i) f.payload[i] = (uint8_t)(len % 3 == 0 ? 0 : len % 3 == 1 ? 0xFF : rand());
        uint8_t w[MAX_WIRE];
        size_t n = encode(f, w);
        CHECK(n > 0 && n <= MAX_WIRE && w[n - 1] == 0);
        CHECK(memchr(w, 0, n - 1) == NULL);  // no zero before the delimiter
        Decoder d = {};
        Frame g;
        CHECK(feed(d, w, n, &g) == 1 && same(f, g));
    }

    // A long run without zeros crosses a 254-byte COBS block.
    Frame big = {DATA, 0, 9, 200, (uint8_t)MAX_PAYLOAD, {}};
    memset(big.payload, 0x11, MAX_PAYLOAD);
    uint8_t w[MAX_WIRE];
    size_t n = encode(big, w);
    Decoder d = {};
    Frame g;
    CHECK(feed(d, w, n, &g) == 1 && same(big, g));

    // Garbage before a frame (a box starting mid-stream): resynchronised at the next 0x00.
    Frame ping = {CTRL, FLAG_ACK_REQ, 3, OP_PING, 0, {}};
    n = encode(ping, w);
    uint8_t noisy[64] = {'h', 'e', 'l', 'l', 'o', '\r', '\n', 0x00};
    memcpy(noisy + 8, w, n);
    d = Decoder{};
    CHECK(feed(d, noisy, 8 + n, &g) == 1 && same(ping, g));
    CHECK(d.dropped == 1);  // "hello\r\n" was dropped

    // A corrupted byte fails the CRC; a truncated frame is too short.
    n = encode(ping, w);
    w[2] ^= 0x40;
    d = Decoder{};
    CHECK(feed(d, w, n, &g) == 0 && d.dropped == 1);
    uint8_t shortf[] = {0x03, 0x24, 0x01, 0x00};
    d = Decoder{};
    CHECK(feed(d, shortf, sizeof shortf, &g) == 0 && d.dropped == 1);

    // Too long a payload isn't encoded; an endless frame is dropped, not overrun.
    Frame huge = big;
    huge.length = MAX_PAYLOAD + 1;
    CHECK(encode(huge, w) == 0);
    d = Decoder{};
    for (int i = 0; i < 1000; ++i) d.feed(0x55, &g);
    CHECK(!d.feed(0x00, &g) && d.dropped == 1);
    n = encode(ping, w);
    CHECK(feed(d, w, n, &g) == 1);  // and the next frame is fine

    printf("%s (%d failures)\n", fails ? "FAILED" : "ok", fails);
    return fails != 0;
}
