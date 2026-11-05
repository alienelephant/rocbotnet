// Where rlink keeps what must survive a power cut: the node identity, the chain
// key with its epoch, and the frame-counter reservation (rlink.cpp).
//
// ESP32: Preferences (NVS), as before. Raspberry Pi Pico (arduino-pico): a
// Store with the same few calls on the core's EEPROM emulation, which is one
// 4 KB flash sector, erased and rewritten whole on every commit. An overwritten
// key is therefore erased, not left behind in an older block as a copy-on-write
// file system would (roclink SECURITY.md R4). rlink commits about once per 256
// frames plus once a day for the key ratchet, far inside flash endurance.
#pragma once

#if defined(ARDUINO_ARCH_RP2040)  // arduino-pico, RP2040 and RP2350 alike
#include <Arduino.h>
#include <EEPROM.h>
#include <string.h>

class Store {
public:
    bool begin(const char *, bool) {
        EEPROM.begin(sizeof(Rec) + 16);
        EEPROM.get(0, rec_);
        if (rec_.magic != MAGIC) {  // blank or foreign: start empty
            memset(&rec_, 0, sizeof(rec_));
            rec_.magic = MAGIC;
        }
        return true;
    }

    uint32_t getULong(const char *k, uint32_t def) {
        if (!strcmp(k, "id")) return rec_.have & HAVE_ID ? rec_.id : def;
        if (!strcmp(k, "fcnt")) return rec_.have & HAVE_FCNT ? rec_.fcnt : def;
        return def;
    }

    size_t putULong(const char *k, uint32_t v) {
        if (!strcmp(k, "id")) {
            rec_.id = v;
            rec_.have |= HAVE_ID;
        } else if (!strcmp(k, "fcnt")) {
            rec_.fcnt = v;
            rec_.have |= HAVE_FCNT;
        } else {
            return 0;
        }
        return commit() ? sizeof(uint32_t) : 0;
    }

    size_t getBytes(const char *k, void *buf, size_t len) {
        if (strcmp(k, "key") || !(rec_.have & HAVE_KEY) || len < sizeof(rec_.key)) return 0;
        memcpy(buf, rec_.key, sizeof(rec_.key));
        return sizeof(rec_.key);
    }

    size_t putBytes(const char *k, const void *buf, size_t len) {
        if (strcmp(k, "key") || len != sizeof(rec_.key)) return 0;
        memcpy(rec_.key, buf, len);
        rec_.have |= HAVE_KEY;
        return commit() ? len : 0;
    }

    bool remove(const char *k) {
        if (!strcmp(k, "fcnt")) rec_.have &= ~HAVE_FCNT;
        else if (!strcmp(k, "id")) rec_.have &= ~HAVE_ID;
        else if (!strcmp(k, "key")) {
            memset(rec_.key, 0, sizeof(rec_.key));
            rec_.have &= ~HAVE_KEY;
        } else {
            return false;
        }
        return commit();
    }

private:
    static constexpr uint32_t MAGIC = 0x524C4B31;  // "RLK1"
    static constexpr uint8_t HAVE_ID = 1, HAVE_KEY = 2, HAVE_FCNT = 4;
    struct Rec {
        uint32_t magic;
        uint8_t have;
        uint32_t id;
        uint8_t key[20];  // epoch u32 + chain key, one record (rlink.cpp store_key)
        uint32_t fcnt;
    };
    bool commit() {
        EEPROM.put(0, rec_);
        return EEPROM.commit();
    }
    Rec rec_{};
};

#else  // ESP32
#include <Preferences.h>
using Store = Preferences;
#endif
