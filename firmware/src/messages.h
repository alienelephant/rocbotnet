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
