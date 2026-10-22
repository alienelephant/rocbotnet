// An example box: sends a reading every 5 s, and reports a win or a wrong
// answer from two buttons. Wire Serial1 to the proxy's box port
// (docs/BOXLINK.md section 1): box TX -> proxy RX, box RX -> proxy TX, GND.
//
// The hub decodes schema 32 with:
//   [decoder.example-reading]
//   struct = "<hfB"
//   fields = ["temp_c", "humidity", "flags"]
//   scale = { temp_c = 0.01 }
//   bits = { flags = ["door_open", "lid_closed"] }
#include <BoxLink.h>

#ifndef PIN_SOLVED
#define PIN_SOLVED 2  // a button to GND: the puzzle is solved
#endif
#ifndef PIN_WRONG
#define PIN_WRONG 3   // a button to GND: a wrong answer
#endif

BoxLink proxy(Serial1);

// Exactly the bytes the hub's struct format describes: packed, little-endian.
struct __attribute__((packed)) Reading {
    int16_t temp_cC;  // centidegrees
    float humidity;   // %
    uint8_t flags;    // bit 0 door open, bit 1 lid closed
};

static void session_changed(bool open) {
    Serial.println(open ? "a team logged in: puzzle armed" : "session over: reset the puzzle");
}

static void verdict(const BoxLink::Verdict &v) {
    Serial.printf("verdict %u (arg %u), %u of %u solved\n", v.result, v.arg, v.solved, v.required);
}

void setup() {
    Serial.begin(115200);
    Serial1.begin(115200);
    pinMode(PIN_SOLVED, INPUT_PULLUP);
    pinMode(PIN_WRONG, INPUT_PULLUP);
    proxy.on_session(session_changed);
    proxy.on_verdict(verdict);
    proxy.begin("example", 1);
}

void loop() {
    static uint32_t last_reading;
    proxy.poll();

    if (millis() - last_reading >= 5000) {
        last_reading = millis();
        Reading r = {2215, 48.5f, 0x02};
        int rc = proxy.send(32, r);
        Serial.printf("reading: %s\n", rc == BoxLink::OK ? "forwarded" : "not forwarded");
    }
    if (proxy.in_session() && digitalRead(PIN_SOLVED) == LOW) {
        proxy.trigger(BoxLink::WIN);
        delay(300);
    }
    if (proxy.in_session() && digitalRead(PIN_WRONG) == LOW) {
        proxy.trigger(BoxLink::FAIL);
        delay(300);
    }
}
