// roclink-scavbox-wifi: a demo puzzle box. Its "puzzle" is a button (BOOT on
// the ESP32 dev board; a push button on D0 in the xiao build):
//
//   every press      box data: how many presses so far, and how long this one was
//                    held. The proxy forwards it unchanged; the hub decodes it.
//   held WIN_HOLD_MS the puzzle is solved: that press is reported, then a WIN for
//                    the team logged in at the proxy (1.5 s; 5 s in the xiao build)
//
// It talks boxlink (rfidprox docs/BOXLINK.md) to an rfidprox on UART2, through
// the BoxLink library. The proxy forwards nothing until a team logs in on it,
// so presses before that are refused ("no_session"). The LED shows the session.
#include <Arduino.h>
#include <BoxLink.h>

#ifndef PIN_BUTTON
#define PIN_BUTTON 0   // BOOT, on the board; pressed is low
#endif
#ifndef PIN_LED
#define PIN_LED 2      // the blue LED on most ESP32-WROOM dev boards; -1 for none
#endif
#ifndef PIN_BOX_RX
#define PIN_BOX_RX 16  // RX2 <- proxy TX
#endif
#ifndef PIN_BOX_TX
#define PIN_BOX_TX 17  // TX2 -> proxy RX
#endif
#ifndef WIN_HOLD_MS
#define WIN_HOLD_MS 1500
#endif
#ifndef LED_ON
#define LED_ON HIGH    // LOW for an LED wired to 3V3 (the XIAO's user LED)
#endif
#ifndef BOX_TYPE
#define BOX_TYPE "scavbox-wifi"  // what HELLO tells the proxy
#endif

BoxLink proxy(Serial2);  // not "link": that's a POSIX function

// Schema 32: exactly what the hub's [decoder.scavbox-press] says, struct "<HH".
struct __attribute__((packed)) Press {
    uint16_t presses;
    uint16_t held_ms;
};
static constexpr uint8_t SCHEMA_PRESS = 32;

static uint16_t presses;

static const char *answer(int rc) {
    switch (rc) {
        case BoxLink::OK: return "OK";
        case BoxLink::NO_ANSWER: return "no answer from the proxy (check the wiring)";
        case boxlink::REFUSED_NO_SESSION: return "refused: no one is logged in at the proxy";
        case boxlink::REFUSED_BUSY: return "refused: busy, try again";
        case boxlink::REFUSED_OFFLINE: return "refused: the proxy has no link to the gateway";
        default: return "refused";
    }
}

static void led(bool on) {
    if (PIN_LED >= 0) digitalWrite(PIN_LED, on == (LED_ON == HIGH) ? HIGH : LOW);
}

static void session_changed(bool open) {
    Serial.println(open ? "[box] a team logged in: play!" : "[box] session over");
    led(open);
}

static void verdict(const BoxLink::Verdict &v) {
    static const char *const RESULTS[] = {"solved", "solved, and finished the hunt", "already solved",
                                          "locked", "unknown team", "not running", "go", "busy",
                                          "no session", "hint granted", "no more hints"};
    Serial.printf("[box] the game says: %s (arg %u), %u of %u boxes solved\n",
                  v.result < 11 ? RESULTS[v.result] : "?", v.arg, v.solved, v.required);
}

// Every press is reported as box data, the winning one included.
static void report_press(uint32_t held_ms) {
    Press p = {(uint16_t)(presses + 1), (uint16_t)min<uint32_t>(held_ms, 0xFFFF)};
    int rc = proxy.send(SCHEMA_PRESS, p);
    if (rc == BoxLink::OK) presses++;  // counted once the proxy took it
    Serial.printf("[box] press %u (%lu ms): %s\n", p.presses, (unsigned long)held_ms, answer(rc));
}

static void solved(uint32_t held_ms) {
    report_press(held_ms);  // the winning press is a press too
    Serial.println("[box] held: the puzzle is solved, sending WIN");
    int rc = proxy.trigger(BoxLink::WIN);
    Serial.printf("[box] WIN: %s\n", answer(rc));
}

void setup() {
    Serial.begin(115200);
    Serial2.begin(115200, SERIAL_8N1, PIN_BOX_RX, PIN_BOX_TX);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    if (PIN_LED >= 0) pinMode(PIN_LED, OUTPUT);
    led(false);
    delay(200);
    Serial.printf("\n[box] %s: the button on GPIO%d is the puzzle (press: data, hold %.1f s: WIN); "
                  "proxy on RX GPIO%d, TX GPIO%d\n", BOX_TYPE, PIN_BUTTON, WIN_HOLD_MS / 1000.0, PIN_BOX_RX, PIN_BOX_TX);
    proxy.on_session(session_changed);
    proxy.on_verdict(verdict);
    if (proxy.begin(BOX_TYPE, 1) != BoxLink::OK)
        Serial.println("[box] proxy: no answer yet (check the UART wiring: box TX -> proxy RX, box RX <- proxy TX, "
                       "GND); the keepalive picks it up once the proxy is running");
    else
        Serial.printf("[box] proxy: answered; its link to the gateway is %s, %s\n",
                      proxy.link_up() ? "up" : "down for now", proxy.in_session() ? "a team is logged in" : "no one logged in");
}

void loop() {
    static bool down, handled;
    static uint32_t since;
    proxy.poll();
    bool pressed = digitalRead(PIN_BUTTON) == LOW;
    if (pressed && !down) {
        down = true;
        handled = false;
        since = millis();
    } else if (pressed && !handled && millis() - since >= WIN_HOLD_MS) {
        handled = true;  // as soon as it's been held long enough
        solved(millis() - since);
    } else if (!pressed && down) {
        down = false;
        uint32_t held = millis() - since;
        if (!handled && held >= 30) report_press(held);  // shorter is contact bounce
    }
}
