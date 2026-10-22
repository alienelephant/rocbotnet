// The proxy's own roclink messages (docs/BACKEND.md section 1). Little-endian;
// every payload starts with a version byte.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace msg {

constexpr uint8_t VERSION = 1;

// schemas, proxy -> rfidauth
constexpr uint8_t HELLO = 1;
constexpr uint8_t AUTH_REQ = 2;
constexpr uint8_t RENEW = 3;
constexpr uint8_t SESSION_END = 4;
constexpr uint8_t STATUS = 5;
// schemas, rfidauth -> proxy
constexpr uint8_t GRANT = 16;
constexpr uint8_t RENEWED = 17;
constexpr uint8_t SESSION_CLOSE = 18;

constexpr uint8_t RADIO_WIFI_UDP = 2;
constexpr uint8_t METHOD_UID = 0;

// HELLO: ver | fw_version u16 | radio u8 | box_type (length-prefixed)
inline size_t hello(uint8_t *out, uint16_t fw, uint8_t radio, const char *box_type) {
    size_t n = 0, bl = box_type ? strlen(box_type) : 0;
    if (bl > 16) bl = 16;
    out[n++] = VERSION;
    out[n++] = (uint8_t)fw;
    out[n++] = (uint8_t)(fw >> 8);
    out[n++] = radio;
    out[n++] = (uint8_t)bl;
    memcpy(out + n, box_type, bl);
    return n + bl;
}

// AUTH_REQ: ver | req u8 | method u8 | atqa u16 | sak u8 | uid_len u8 | uid
inline size_t auth_req(uint8_t *out, uint8_t req, uint16_t atqa, uint8_t sak, const uint8_t *uid,
                       uint8_t uid_len) {
    size_t n = 0;
    out[n++] = VERSION;
    out[n++] = req;
    out[n++] = METHOD_UID;
    out[n++] = (uint8_t)atqa;
    out[n++] = (uint8_t)(atqa >> 8);
    out[n++] = sak;
    out[n++] = uid_len;
    memcpy(out + n, uid, uid_len);
    return n + uid_len;
}

}  // namespace msg

// The scavenger hunt's own box messages (roclink-scavhunt docs/GAME.md 0.2
// section 6), for the game-box build (gamebox.cpp, docs/BOXLINK.md section 9): the proxy
// sends them for its box, with the logged-in fob as the team's token.
namespace game {

constexpr uint8_t VERSION = 1;

// schemas, box -> game
constexpr uint8_t WIN = 1;
constexpr uint8_t START = 2;
constexpr uint8_t WRONG = 3;
constexpr uint8_t HINT = 4;
constexpr uint8_t END = 5;
// schemas, game -> box
constexpr uint8_t VERDICT = 10;  // ver | result | solved | required | arg u16
constexpr uint8_t CONTROL = 11;  // ver | command (1 reset, 2 disable, 3 enable)

// ver | token length | token | hint number (HINT only; pass hint 0 otherwise)
inline size_t box_message(uint8_t *out, const uint8_t *token, uint8_t token_len, uint8_t hint) {
    size_t n = 0;
    out[n++] = VERSION;
    out[n++] = token_len;
    memcpy(out + n, token, token_len);
    n += token_len;
    if (hint) out[n++] = hint;
    return n;
}

}  // namespace game
