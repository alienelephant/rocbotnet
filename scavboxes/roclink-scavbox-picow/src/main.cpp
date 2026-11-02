// roclink-scavbox-picow: a demo puzzle box on a Raspberry Pi Pico 2 W. Its
// "puzzle" is the BOOTSEL button:
//
//   short press      box data: how many presses so far, and how long this one was
//                    held. The proxy forwards it unchanged; the hub decodes it.
//   held 1.5 s       the puzzle is solved: a WIN for the team logged in at the proxy
//
// It talks boxlink (rfidprox docs/BOXLINK.md) to an rfidprox on UART0, through
// the BoxLink library: the same box as roclink-scavbox-wifi, on another board,
// so the hub's "scavbox" device type decodes it as it is. The Pico 2 W's own
// Wi-Fi isn't used: the proxy is the box's only way to the game, and it
// forwards nothing until a team logs in on it, so presses before that are
// refused ("no_session"). The LED shows the session.
#include <Arduino.h>
#include <BoxLink.h>

// The board's own Serial1 pins unless told otherwise: on the Pico 2 W (and the
// Pico W), UART0 on GP0 and GP1.
#ifndef PIN_BOX_TX
#define PIN_BOX_TX PIN_SERIAL1_TX  // GP0, pin 1: UART0 TX -> proxy RX
#endif
#ifndef PIN_BOX_RX
#define PIN_BOX_RX PIN_SERIAL1_RX  // GP1, pin 2: UART0 RX <- proxy TX
#endif
// The puzzle: BOOTSEL by default. Build with -D PIN_BUTTON=21 for a push
// button from that GPIO to GND instead.
#define WIN_HOLD_MS 1500
#define HELLO_RETRY_MS 5000

BoxLink proxy(Serial1);  // not "link": that's a POSIX function

// Schema 32: exactly what the hub's [decoder.scavbox-press] says, struct "<HH".
struct __attribute__((packed)) Press {
    uint16_t presses;
    uint16_t held_ms;
};
static constexpr uint8_t SCHEMA_PRESS = 32;

static uint16_t presses;
static bool proxy_answered;
static uint32_t last_hello_ms;

static bool pressed() {
#ifdef PIN_BUTTON
    return digitalRead(PIN_BUTTON) == LOW;
#else
    return BOOTSEL;  // reading it pauses flash for a moment: fine for a button
#endif
}

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

static void session_changed(bool open) {
    Serial.println(open ? "[box] a team logged in: play!" : "[box] session over");
    digitalWrite(LED_BUILTIN, open ? HIGH : LOW);
}

// Called from inside send() and trigger() as well as poll(): only log here,
// never send from a handler.
static void verdict(const BoxLink::Verdict &v) {
    static const char *const RESULTS[] = {"solved", "solved, and finished the hunt", "already solved",
                                          "locked", "unknown team", "not running", "go", "busy",
                                          "no session", "hint granted", "no more hints"};
    Serial.printf("[box] the game says: %s (arg %u), %u of %u boxes solved\n",
                  v.result < 11 ? RESULTS[v.result] : "?", v.arg, v.solved, v.required);
}

static void data_from_hub(uint8_t schema, const uint8_t *data, size_t length) {
    Serial.printf("[box] data from the hub on schema %u, %u bytes\n", schema, (unsigned)length);
    (void)data;
}

static void hello() {
    last_hello_ms = millis();
    if (proxy.begin("scavbox-picow", 1) != BoxLink::OK) {
        Serial.printf("[box] proxy: no answer (check the UART wiring: GP%d -> proxy RX, GP%d <- proxy TX, "
                      "GND); trying again in %d s\n", PIN_BOX_TX, PIN_BOX_RX, HELLO_RETRY_MS / 1000);
        return;
    }
    proxy_answered = true;
    Serial.printf("[box] proxy: answered; its link to the gateway is %s, %s\n",
                  proxy.link_up() ? "up" : "down for now",
                  proxy.in_session() ? "a team is logged in" : "no one logged in");
}

static void short_press(uint32_t held_ms) {
    Press p = {(uint16_t)(presses + 1), (uint16_t)(held_ms > 0xFFFF ? 0xFFFF : held_ms)};
    int rc = proxy.send(SCHEMA_PRESS, p);
    if (rc == BoxLink::OK) presses++;  // counted once the proxy took it
    Serial.printf("[box] press %u (%lu ms): %s\n", p.presses, (unsigned long)held_ms, answer(rc));
}

static void solved() {
    Serial.println("[box] held: the puzzle is solved, sending WIN");
    int rc = proxy.trigger(BoxLink::WIN);
    Serial.printf("[box] WIN: %s\n", answer(rc));
}

void setup() {
    Serial.begin(115200);  // USB: the log
    Serial1.setTX(PIN_BOX_TX);
    Serial1.setRX(PIN_BOX_RX);
    Serial1.begin(115200);
#ifdef PIN_BUTTON
    pinMode(PIN_BUTTON, INPUT_PULLUP);
#endif
    pinMode(LED_BUILTIN, OUTPUT);  // on the Pico 2 W, the LED hangs off the Wi-Fi chip
    digitalWrite(LED_BUILTIN, LOW);
    // BoxLink picks its first seq with random(): seed it, or every restart
    // starts from the same one.
    randomSeed(rp2040.hwrand32());
    delay(1500);  // a moment to open the USB monitor
    Serial.printf("\n[box] scavbox-picow: the button is the puzzle (press: data, hold %.1f s: WIN); "
                  "proxy on UART0, TX GP%d, RX GP%d\n", WIN_HOLD_MS / 1000.0, PIN_BOX_TX, PIN_BOX_RX);
    proxy.on_session(session_changed);
    proxy.on_verdict(verdict);
    proxy.on_data(data_from_hub);
    hello();
}

void loop() {
    static bool down, handled;
    static uint32_t since;
    proxy.poll();
    // Until a proxy answers HELLO, say it again now and then: it may have
    // started after the box.
    if (!proxy_answered && millis() - last_hello_ms >= HELLO_RETRY_MS) hello();

    bool now = pressed();
    if (now && !down) {
        down = true;
        handled = false;
        since = millis();
    } else if (now && !handled && millis() - since >= WIN_HOLD_MS) {
        handled = true;  // as soon as it's been held long enough
        solved();
    } else if (!now && down) {
        down = false;
        uint32_t held = millis() - since;
        if (!handled && held >= 30) short_press(held);  // shorter is contact bounce
    }
}
