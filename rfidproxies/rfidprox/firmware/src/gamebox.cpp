// rfidprox, game-box build (the *_gamebox environments): the proxy as a
// scavhunt box's link to the game, for the demo.
//
// The box speaks boxlink (docs/BOXLINK.md, the BoxLink library in ../boxlink)
// on the box port. Its own data (DATA, schemas 32-255) is forwarded unchanged
// as secured roclink, for the hub to decode. Its game events (CTRL TRIGGER:
// win, fail, hint, end) go to the game as GAME.md 0.2 messages, with the token
// of whoever logged in. The game's verdicts and the session come back to it.
//
// No card reader: a button stands in for a fob (PRG on the Heltec, a push
// button on D0 on the XIAO). A short press logs in with the dummy fob
// (DUMMY_FOB in config.h); held 1.5 s, it logs out. Box data and game events
// are only forwarded while someone is logged in.
//
// Built with AUTO_LOGIN=1 (xiao_gamebox_auto), it needs no button: it logs in
// with the dummy fob as soon as its link is up, and tries again every
// AUTO_LOGIN_RETRY_MS until the game says go (the run may not be started yet,
// or the team not entered), and whenever the session ends without the box
// solved. Once it's solved, it stays logged out until it restarts.
//
// The Heltec shows the state on its OLED. The XIAO has no display: its user LED
// blinks with no link, is off when ready, flickers while waiting for the game,
// and stays on while someone is logged in; the serial log has the details.
//
// Built with LOGIN_FOB=1 (pico2w_fobproxy) there's no button and no dummy
// fob: a 125 kHz fob tapped on the RDM6300 logs in, and its 5-byte ID is the
// team's token (enter it in the game: roclink scavhunt team ... --token
// <10 hex digits>). Fobs log in: the box ends a session (solved, gave up), and
// so does another fob tapped while it's open (that team gets an END, the box a
// SESSION_CLOSED "revoked"), so a team that walks away never locks the box.
// The reader is parsed by RDM6300Wrapper (lib/).
//
// The USB serial port is the log. Once a boxlink frame arrives there too (from
// tools/boxsim.py), the proxy also sends the box's frames there, between log lines.
#include <Arduino.h>
#include <boxlink_frame.h>

#include "config.h"
#include "messages.h"
#include "rlink.h"

#if LOGIN_FOB
#include <RDM6300Wrapper.h>
#endif
#if defined(ARDUINO_ARCH_RP2040)
#include <hardware/gpio.h>
#endif

#if HAS_OLED
#include <U8g2lib.h>
#include <Wire.h>
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, PIN_OLED_RST, PIN_OLED_SCL, PIN_OLED_SDA);
#endif

using boxlink::Frame;

static HardwareSerial &box = Serial1;

static constexpr uint32_t VERDICT_TIMEOUT_MS = 10000;
static constexpr uint32_t DATA_MIN_MS = 250;   // BOXLINK.md 9: at most four DATA frames a second
static constexpr uint32_t TRIGGER_MIN_MS = 1000;  // and one trigger a second: each WRONG costs the team time
static constexpr uint32_t REPEAT_MS = 10000;   // a seq seen again this soon is the box's retry
static constexpr uint32_t LOGOUT_HOLD_MS = 1500;
static constexpr uint32_t AUTO_LOGIN_RETRY_MS = 15000;
static constexpr uint8_t NO_ACK = 0xFF;        // handle(): answered some other way

// --- state
enum class State { joining, linking, idle, waiting };  // waiting: for the game's verdict
static State state = State::joining;
static uint32_t last_ping_ms, link_retry_ms, sent_ms, last_data_ms, last_trigger_ms;
static uint16_t session, next_session = 1;  // the session id while someone is logged in, else 0
static uint8_t hints_taken;
static bool auto_login = AUTO_LOGIN;  // still to log in by itself: cleared once the box is solved
#if !LOGIN_FOB
static bool auto_tried;
static uint32_t auto_login_ms;        // its last try
#endif
// The token logins send: the dummy fob, or with LOGIN_FOB the ID of the fob
// that was tapped. It stays for the whole session: triggers carry it.
#if LOGIN_FOB
static uint8_t fob[5];
static uint8_t fob_len;
#define GO_DETAIL "fob logged in"
#define LOGIN_PROMPT "Tap a fob", "to log in"
#define LOGIN_DETAIL "tap a fob"
#else
static const uint8_t fob[] = DUMMY_FOB;
static const uint8_t fob_len = sizeof(fob);
#define GO_DETAIL "hold button: log out"
#define LOGIN_PROMPT "Press button", "to log in"
#define LOGIN_DETAIL "press the button"
#endif
static char headline[22] = "Starting";
static char detail[22] = "";

static bool link_up() { return state == State::idle || state == State::waiting; }

// ------------------------------------------------------------------- display

static void beep(int freq, int ms) {
#ifdef PIN_BUZZER
    tone(PIN_BUZZER, freq, ms);
#else
    (void)freq;
    (void)ms;
#endif
}

static void show(const char *line1, const char *line2 = "") {
#if !HAS_OLED  // no display: what it would say goes to the log
    if (strcmp(headline, line1) || strcmp(detail, line2))
        Serial.printf("[show] %s%s%s\n", line1, *line2 ? ": " : "", line2);
#endif
    snprintf(headline, sizeof(headline), "%s", line1);
    snprintf(detail, sizeof(detail), "%s", line2);
}

#if HAS_OLED
static void draw() {
    static uint32_t last_draw;
    if (millis() - last_draw < 250) return;
    last_draw = millis();
    const rlink::Stats &s = rlink::stats();
    char l1[24], l2[24], l3[24];
    snprintf(l1, sizeof(l1), "scavbox %04X  %s", ROCLINK_NODE, session ? "IN" : "out");
    if (!s.last_ack_ms) snprintf(l2, sizeof(l2), "tx %lu, no ACK yet", (unsigned long)s.sent);
    else snprintf(l2, sizeof(l2), "tx %lu, ACK %lus ago", (unsigned long)s.sent,
                  (unsigned long)((millis() - s.last_ack_ms) / 1000));
    snprintf(l3, sizeof(l3), "%s", rlink::describe());  // Wi-Fi address, or LoRa signal
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(0, 9, l1);
    oled.drawStr(0, 20, l2);
    oled.drawStr(0, 31, l3);
    oled.drawHLine(0, 34, 128);
    oled.setFont(u8g2_font_7x13B_tf);
    oled.drawStr(0, 49, headline);
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(0, 62, detail);
    oled.sendBuffer();
}
#else
// The user LED: blinking 1 Hz with no link, off when ready, flickering while
// waiting for the game's verdict, on while someone is logged in.
static void draw() {
    bool on;
    if (state == State::joining || state == State::linking) on = millis() / 500 % 2;
    else if (state == State::waiting) on = millis() / 80 % 2;
    else on = session != 0;
    digitalWrite(PIN_LED, on == (LED_ON == HIGH) ? HIGH : LOW);  // LED_ON is LOW on the XIAO
}
#endif

// ------------------------------------------------------------------ to the box

enum class Port { box, usb };
static bool usb_box;  // a box (tools/boxsim.py) speaks boxlink on the USB port too
static uint8_t out_seq;

static void transmit(Port to, const Frame &f) {
    uint8_t wire[boxlink::MAX_WIRE];
    size_t n = boxlink::encode(f, wire);
    Stream &s = to == Port::box ? (Stream &)box : (Stream &)Serial;
    s.write((uint8_t)0x00);  // a delimiter first: the receiver drops anything half-read
    s.write(wire, n);
}

// Sends to the box, and to the USB port if a box speaks there.
static void to_box(uint8_t type, uint8_t schema, const uint8_t *p, size_t n) {
    Frame f = {type, 0, ++out_seq, schema, (uint8_t)n, {}};
    memcpy(f.payload, p, n);
    transmit(Port::box, f);
    if (usb_box) transmit(Port::usb, f);
}

static void ack(Port to, uint8_t seq, uint8_t refusal) {
    Frame f = {boxlink::ACK, 0, seq, 0, (uint8_t)(refusal ? 1 : 0), {refusal}};
    transmit(to, f);
}

static void send_status() {
    uint8_t link = link_up() ? boxlink::LINK_UP : boxlink::LINK_DOWN;
    uint8_t p[4] = {link, (uint8_t)session, (uint8_t)(session >> 8), 0xFF};  // no rate budget yet
    to_box(boxlink::CTRL, boxlink::OP_STATUS, p, sizeof p);
}

// Session close reasons (BOXLINK.md 4.2)
static constexpr uint8_t CLOSED_BOX_ENDED = 1, CLOSED_REVOKED = 2, CLOSED_EXPIRED = 3;

static void open_session() {
    if (session) return;
    session = next_session++;
    if (!next_session) next_session = 1;
    char label[24];
    int ll = snprintf(label, sizeof(label), "fob ");
    for (uint8_t i = 0; i < fob_len; ++i) ll += snprintf(label + ll, sizeof(label) - ll, "%02x", fob[i]);
    uint8_t p[5 + sizeof(label)] = {(uint8_t)session, (uint8_t)(session >> 8), 0, 0, (uint8_t)ll};
    memcpy(p + 5, label, ll);
    Serial.printf("[boxlink] < SESSION_OPEN %u\n", session);
    to_box(boxlink::CTRL, boxlink::OP_SESSION_OPEN, p, 5 + ll);
}

static void close_session(uint8_t reason) {
    if (!session) return;
    uint8_t p[3] = {(uint8_t)session, (uint8_t)(session >> 8), reason};
    Serial.printf("[boxlink] < SESSION_CLOSED %u, reason %u\n", session, reason);
    session = 0;
    to_box(boxlink::CTRL, boxlink::OP_SESSION_CLOSED, p, sizeof p);
}

static void set_state(State s) {
    bool was = link_up();
    state = s;
    if (link_up() != was) {
        Serial.printf("[boxlink] < STATUS link %s\n", link_up() ? "up" : "down");
        send_status();
    }
}

// ------------------------------------------------------------------ downlink

static void on_verdict(const uint8_t *p, size_t len) {
    if (len < 6 || p[0] != game::VERSION) return;
    uint8_t result = p[1], solved = p[2], required = p[3];
    uint16_t arg = (uint16_t)(p[4] | p[5] << 8);
    Serial.printf("[game] VERDICT result %u, solved %u of %u, arg %u\n", result, solved, required, arg);
    if (state == State::waiting) set_state(State::idle);
    uint8_t v[5] = {result, (uint8_t)arg, (uint8_t)(arg >> 8), solved, required};
    to_box(boxlink::CTRL, boxlink::OP_VERDICT, v, sizeof v);
    char l2[22];
    snprintf(l2, sizeof(l2), "%u of %u boxes", solved, required);
    bool good = true;
    switch (result) {
        case 0: show("Solved!", l2); break;
        case 1: show("FINISHED!", l2); break;
        case 2: show("Already solved", l2); break;
        case 3: snprintf(l2, sizeof(l2), "solve %04X first", arg); show("Locked", l2); good = false; break;
        case 4: show("Unknown team", "fob not entered"); good = false; break;
        case 5: show("Not running", "ask the operator"); good = false; break;
        case 6: show("Go!", GO_DETAIL); break;
        case 7: snprintf(l2, sizeof(l2), "free in %u s", arg); show("Busy", l2); good = false; break;
        case 8: show("No session", LOGIN_DETAIL); good = false; break;
        case 9: snprintf(l2, sizeof(l2), "show hint %u", arg); show("Hint granted", l2); break;
        case 10: snprintf(l2, sizeof(l2), "this box has %u", arg); show("No more hints", l2); good = false; break;
        default: snprintf(l2, sizeof(l2), "result %u", result); show("Verdict?", l2); good = false; break;
    }
    if (result == 6) {
        hints_taken = (uint8_t)arg;
        open_session();
    } else if (result == 9 && arg > hints_taken) {
        hints_taken = (uint8_t)arg;
    } else if (result <= 2) {  // solved: the box resets for the next team
        if (auto_login) Serial.println("[game] solved: not logging in again until restarted");
        auto_login = false;    // or it would log in, hear "already solved", and again
        close_session(CLOSED_BOX_ENDED);
    } else if (result == 8) {  // the game has no session for this team
        close_session(CLOSED_EXPIRED);
    }
    beep(good ? 2600 : 400, good ? 120 : 300);
}

static void on_downlink(uint8_t schema, const uint8_t *p, size_t len) {
    if (schema == game::VERDICT) return on_verdict(p, len);
    if (schema == game::CONTROL && len >= 2 && p[0] == game::VERSION) {
        Serial.printf("[game] CONTROL %u\n", p[1]);
        if (p[1] == 1) {
            show("Reset", "by the operator");
            close_session(CLOSED_REVOKED);
        } else {
            show(p[1] == 2 ? "Disabled" : "Enabled", "by the operator");
        }
        return;
    }
    if (schema >= boxlink::FIRST_BOX_SCHEMA && len <= boxlink::MAX_PAYLOAD) {  // the box's own: pass it on
        Serial.printf("[boxlink] < DATA schema %u, %u bytes (from the hub)\n", schema, (unsigned)len);
        return to_box(boxlink::DATA, schema, p, len);
    }
    Serial.printf("[link] downlink schema %u not handled\n", schema);
}

// -------------------------------------------------------------------- uplink

static const char *const NAMES[] = {"", "WIN", "START", "WRONG", "HINT", "END"};

// Sends one game message with the fob's token. START, WIN and HINT then wait
// for their verdict. False if the gateway didn't ACK it.
static bool game_send(uint8_t schema, uint8_t hint) {
    uint8_t payload[24];
    size_t n = game::box_message(payload, fob, fob_len, hint);
    show("Sending...", NAMES[schema]);
    draw();
    // Waiting from before it goes out: the verdict can arrive while rlink::send()
    // still waits for the gateway's ACK, and must find us waiting for it.
    if (schema == game::START || schema == game::WIN || schema == game::HINT) {
        set_state(State::waiting);
        sent_ms = millis();
    }
    if (!rlink::send(schema, payload, n, true)) {
        Serial.printf("[game] %s: no ACK from the gateway\n", NAMES[schema]);
        show("No link", NAMES[schema]);
        beep(400, 300);
        set_state(State::linking);
        return false;
    }
    Serial.printf("[game] %s sent and ACKed\n", NAMES[schema]);
    return true;
}

#if LOGIN_FOB
// A fob was tapped: log in with it. If another fob's session is open, that
// session ends first (END to the game for that team, SESSION_CLOSED "revoked"
// to the box), so a team that walked away never locks the box. The fob that
// is already logged in, tapped again, changes nothing, and so does any tap
// while a login is waiting for the game's verdict.
static void login_fob(const uint8_t id[5], const char *hex) {
    if (!link_up()) return show("Offline", "can't log in");
    if (state == State::waiting) {
        Serial.printf("[fob] %s tapped: ignored, still waiting for the game\n", hex);
        return;
    }
    if (session && fob_len == 5 && !memcmp(fob, id, 5)) {
        Serial.printf("[fob] %s tapped: already logged in (session %u)\n", hex, session);
        return show("Logged in", "box ends it");
    }
    if (session) {
        Serial.printf("[fob] %s tapped: ending session %u first (fob ", hex, session);
        for (uint8_t i = 0; i < fob_len; ++i) Serial.printf("%02X", fob[i]);
        Serial.println(")");
        bool ended = game_send(game::END, 0);  // with the old fob's token
        close_session(CLOSED_REVOKED);
        if (!ended) return;  // no link: game_send() said so; the game times that session out
    }
    memcpy(fob, id, 5);
    fob_len = 5;
    Serial.printf("[game] fob %s: logging in\n", hex);
    game_send(game::START, 0);
}
#else
static void login() {
    if (!link_up()) return show("Offline", "can't log in");
    if (state == State::waiting) return;  // one at a time
    Serial.println("[game] button: logging in with the dummy fob");
    game_send(game::START, 0);
}

static void logout() {
    if (!session) return show("Logged out", "");
    if (state != State::idle) return;
    Serial.println("[game] button held: logging out");
    if (game_send(game::END, 0)) show("Logged out", "");
    close_session(CLOSED_REVOKED);
}
#endif

// A game event from the box (OP_TRIGGER or OP_END_SESSION): 0, or a refusal.
static uint8_t on_trigger(uint8_t what, uint8_t arg) {
    uint8_t schema;
    switch (what) {
        case boxlink::TRIGGER_WIN: schema = game::WIN; break;
        case boxlink::TRIGGER_FAIL: schema = game::WRONG; break;
        case boxlink::TRIGGER_HINT: schema = game::HINT; break;
        case boxlink::TRIGGER_END: schema = game::END; break;
        default: return boxlink::REFUSED_UNKNOWN;
    }
    if (!session) return boxlink::REFUSED_NO_SESSION;
    if (state == State::waiting) return boxlink::REFUSED_BUSY;
    if (!link_up()) return boxlink::REFUSED_OFFLINE;
    if (last_trigger_ms && millis() - last_trigger_ms < TRIGGER_MIN_MS) return boxlink::REFUSED_BUSY;
    last_trigger_ms = millis();
    uint8_t hint = schema == game::HINT ? (arg ? arg : (uint8_t)(hints_taken + 1)) : 0;
    if (!game_send(schema, hint)) return boxlink::REFUSED_OFFLINE;
    if (schema == game::WRONG) show("Wrong answer", "costs time");
    if (schema == game::END) {
        show("Gave up", "");
        close_session(CLOSED_BOX_ENDED);
    }
    return 0;
}

// One frame from the box: does it, and returns 0 (ACK), a refusal, or NO_ACK.
static uint8_t handle(Port from, const Frame &f) {
    if (f.type == boxlink::DATA) {
        Serial.printf("[boxlink] > DATA schema %u, %u bytes\n", f.schema, f.length);
        if (f.schema < boxlink::FIRST_BOX_SCHEMA) return boxlink::REFUSED_BAD_SCHEMA;
        if (!session) return boxlink::REFUSED_NO_SESSION;  // nothing leaves without a login
        if (!link_up()) return boxlink::REFUSED_OFFLINE;
        if (last_data_ms && millis() - last_data_ms < DATA_MIN_MS) return boxlink::REFUSED_BUSY;
        last_data_ms = millis();
        if (!rlink::send(f.schema, f.payload, f.length, true)) {  // unchanged: the hub decodes it
            set_state(State::linking);
            return boxlink::REFUSED_OFFLINE;
        }
        return 0;
    }
    if (f.type != boxlink::CTRL) return NO_ACK;  // an ACK: the proxy asks for none
    const uint8_t *p = f.payload;
    switch (f.schema) {
        case boxlink::OP_HELLO: {
            size_t tl = f.length >= 2 && p[1] <= f.length - 2 ? p[1] : 0;
            Serial.printf("[boxlink] > HELLO from \"%.*s\"\n", (int)tl, (const char *)p + 2);
            uint8_t a[7] = {boxlink::VERSION, (uint8_t)FW_VERSION, (uint8_t)(FW_VERSION >> 8),
                            (uint8_t)boxlink::MAX_PAYLOAD, link_up() ? boxlink::LINK_UP : boxlink::LINK_DOWN,
                            (uint8_t)session, (uint8_t)(session >> 8)};
            Frame r = {boxlink::CTRL, 0, ++out_seq, boxlink::OP_HELLO_ACK, sizeof a, {}};
            memcpy(r.payload, a, sizeof a);
            transmit(from, r);
            return 0;
        }
        case boxlink::OP_PING: {
            uint8_t s[4] = {link_up() ? boxlink::LINK_UP : boxlink::LINK_DOWN, (uint8_t)session,
                            (uint8_t)(session >> 8), 0xFF};
            Frame r = {boxlink::CTRL, 0, ++out_seq, boxlink::OP_STATUS, sizeof s, {}};
            memcpy(r.payload, s, sizeof s);
            transmit(from, r);
            return 0;
        }
        case boxlink::OP_TRIGGER:
            Serial.printf("[boxlink] > TRIGGER %u %u\n", f.length ? p[0] : 0, f.length > 1 ? p[1] : 0);
            if (!f.length) return boxlink::REFUSED_UNKNOWN;
            return on_trigger(p[0], f.length > 1 ? p[1] : 0);
        case boxlink::OP_END_SESSION:
            Serial.println("[boxlink] > END_SESSION");
            if (f.length >= 2 && (uint16_t)(p[0] | p[1] << 8) != session) return boxlink::REFUSED_STALE;
            return on_trigger(boxlink::TRIGGER_END, 0);
        default:
            Serial.printf("[boxlink] > CTRL op %u: unknown\n", f.schema);
            return boxlink::REFUSED_UNKNOWN;
    }
}

// The box's retries carry the same seq: answered again, never done twice.
struct Last {
    bool valid;
    uint8_t seq, answer;
    uint32_t at;
};

static void on_frame(Port from, const Frame &f) {
    static Last last[2];
    Last &l = last[from == Port::box ? 0 : 1];
    bool wants_ack = f.flags & boxlink::FLAG_ACK_REQ;
    if (f.type == boxlink::CTRL && f.schema == boxlink::OP_HELLO) l.valid = false;  // the box (re)started
    if (wants_ack && l.valid && f.seq == l.seq && millis() - l.at < REPEAT_MS) {
        Serial.printf("[boxlink] > seq %u again: answering, not repeating it\n", f.seq);
        return ack(from, f.seq, l.answer);
    }
    uint8_t answer = handle(from, f);
    if (answer == NO_ACK || !wants_ack) return;
    if (answer) Serial.printf("[boxlink] < refused seq %u: %u\n", f.seq, answer);
    ack(from, f.seq, answer);
    l = {true, f.seq, answer, millis()};
}

#if LOGIN_FOB
// ------------------------------------------------------------------ the reader

// The RDM6300 on its own UART, through the 5 V to 3.3 V divider. It sends a
// tag's frame over and over while the tag is in the field, and nothing
// without one; RDM6300Wrapper turns that into one event per tap (again after
// the tag has been away SAME_CARD_MS).
static HardwareSerial &reader_uart = Serial2;
static RDM6300Wrapper reader(reader_uart, SAME_CARD_MS);

// The reader's TX idles high, so through the divider the pin should sit at
// 3.3 V. A weak pull-down makes a line nothing drives read low instead.
static bool reader_line_high() {
#if defined(ARDUINO_ARCH_RP2040)
    return gpio_get(PIN_RDM6300_RX);
#else
    return digitalRead(PIN_RDM6300_RX) == HIGH;
#endif
}

static const char *line_verdict(bool high) {
    return high ? "HIGH: the reader is powered and its TX reaches the pin"
                : "LOW: nothing drives it (reader unpowered, or TX not wired through the divider)";
}

static void reader_begin() {
#if defined(ARDUINO_ARCH_RP2040)
    Serial2.setRX(PIN_RDM6300_RX);
    Serial2.setTX(PIN_RDM6300_TX);  // UART1 wants both; nothing is connected to it
    Serial2.begin(RDM6300Wrapper::BAUD);
    gpio_set_pulls(PIN_RDM6300_RX, false, true);
#else
    reader_uart.begin(RDM6300Wrapper::BAUD, SERIAL_8N1, PIN_RDM6300_RX, -1);
    gpio_pullup_dis((gpio_num_t)PIN_RDM6300_RX);
    gpio_pulldown_en((gpio_num_t)PIN_RDM6300_RX);
#endif
    delay(5);
    Serial.printf("[rdm6300] TX to GPIO%d through the divider, %lu baud; line %s\n", PIN_RDM6300_RX,
                  (unsigned long)RDM6300Wrapper::BAUD, line_verdict(reader_line_high()));
}

// Every RDM6300_REPORT_MS: what arrived from the reader or, with nothing, the line's level.
static void reader_report() {
#if RDM6300_DEBUG
    static uint32_t last_ms, last_bytes, last_frames, last_bad;
    if (millis() - last_ms < RDM6300_REPORT_MS) return;
    last_ms = millis();
    uint32_t bytes = reader.bytes() - last_bytes, frames = reader.frames() - last_frames,
             bad = reader.badFrames() - last_bad;
    last_bytes = reader.bytes();
    last_frames = reader.frames();
    last_bad = reader.badFrames();
    if (!bytes) {
        Serial.printf("[rdm6300] last %lus: no bytes (normal without a tag); line %s\n",
                      (unsigned long)(RDM6300_REPORT_MS / 1000), line_verdict(reader_line_high()));
        return;
    }
    Serial.printf("[rdm6300] last %lus: %lu bytes, %lu tag frames, %lu bad frames\n",
                  (unsigned long)(RDM6300_REPORT_MS / 1000), (unsigned long)bytes, (unsigned long)frames,
                  (unsigned long)bad);
#endif
}

static void read_fob() {
    if (reader.read()) {
        uint8_t id[5];
        reader.lastCardBytes(id);
        Serial.printf("[rdm6300] fob %s (EM4100: id %lu)\n", reader.lastCardHex(),
                      (unsigned long)reader.lastCardNumber());
        login_fob(id, reader.lastCardHex());
    }
    reader_report();
}
#else
// The login button: a short press logs in, held LOGOUT_HOLD_MS logs out.
static void read_button() {
    static bool down, handled;
    static uint32_t since;
    bool pressed = digitalRead(PIN_BUTTON) == LOW;
    if (pressed && !down) {
        down = true;
        handled = false;
        since = millis();
    } else if (pressed && !handled && millis() - since >= LOGOUT_HOLD_MS) {
        handled = true;  // log out as soon as it's been held long enough
        logout();
    } else if (!pressed && down) {
        down = false;
        if (!handled && millis() - since >= 30) login();  // shorter is contact bounce
    }
}
#endif

static void read_frames() {
    static boxlink::Decoder from_box, from_usb;
    Frame f;
    while (box.available())
        if (from_box.feed((uint8_t)box.read(), &f)) on_frame(Port::box, f);
    while (Serial.available()) {
        if (!from_usb.feed((uint8_t)Serial.read(), &f)) continue;
        if (!usb_box) Serial.println("[boxlink] a box on the USB port: sending its frames here too");
        usb_box = true;
        on_frame(Port::usb, f);
    }
}

// ------------------------------------------------------------------- arduino

void setup() {
    Serial.begin(115200);
#if defined(ARDUINO_ARCH_RP2040)
    Serial1.setTX(PIN_BOX_TX);
    Serial1.setRX(PIN_BOX_RX);
    Serial1.begin(BOX_BAUD);
#else
    box.begin(BOX_BAUD, SERIAL_8N1, PIN_BOX_RX, PIN_BOX_TX);
#endif
    delay(200);
    Serial.printf("\n[scavbox] firmware %04X, node %02X/%04X, ", FW_VERSION, ROCLINK_NET, ROCLINK_NODE);
#if LOGIN_FOB
    Serial.print("logins: a tapped fob");
#else
    Serial.print("dummy fob ");
    for (uint8_t i = 0; i < fob_len; ++i) Serial.printf("%02x", fob[i]);
#endif
    Serial.printf("; box port RX GPIO%d, TX GPIO%d\n", PIN_BOX_RX, PIN_BOX_TX);

#if HAS_OLED
    pinMode(PIN_VEXT, OUTPUT);
    digitalWrite(PIN_VEXT, VEXT_ON);  // power the OLED
    delay(50);
    oled.begin();
#else
    pinMode(PIN_LED, OUTPUT);
#endif
#ifdef PIN_BUZZER
    pinMode(PIN_BUZZER, OUTPUT);
#endif
#if LOGIN_FOB
    reader_begin();
#else
    pinMode(PIN_BUTTON, INPUT_PULLUP);
#endif

    if (!rlink::begin(on_downlink)) show("Security failed", "see serial log");
    else show("Starting", rlink::describe());
    send_status();
}

void loop() {
    rlink::loop();
    if (!rlink::ready() && state != State::joining) {
        show("Link lost", "rejoining");
        set_state(State::joining);
    }
    switch (state) {
        case State::joining:
            if (rlink::ready()) set_state(State::linking);
            break;
        case State::linking:  // a PING every 3 s until the gateway answers
            if (link_retry_ms && millis() - link_retry_ms < 3000) break;
            if (rlink::ping()) {
                Serial.println("[scavbox] PING ACKed: ready");
                if (session) show("Linked", "session kept");
                else if (auto_login) show("Linked", "logging in");
                else show(LOGIN_PROMPT);
                link_retry_ms = 0;
                last_ping_ms = millis();
                set_state(State::idle);
            } else {
                Serial.println("[scavbox] PING not ACKed: is roclink-gw reachable at the gateway address?");
                show("No gateway", "retrying");
                link_retry_ms = millis();
            }
            break;
        case State::waiting:
            if (millis() - sent_ms >= VERDICT_TIMEOUT_MS) {
                Serial.println("[game] no VERDICT: are the hub and scavhunt running?");
                show("No answer", "hub or game down?");
                beep(400, 200);
                set_state(State::idle);
            }
            [[fallthrough]];
        case State::idle:
            if (millis() - last_ping_ms >= HEARTBEAT_IDLE_MS) {
                last_ping_ms = millis();
                bool ok = rlink::ping();
                Serial.printf("[link] PING: %s\n", ok ? "ACKed" : "no ACK");
                if (!ok) set_state(State::linking);
            }
#if !LOGIN_FOB
            if (state == State::idle && auto_login && !session &&
                (!auto_tried || millis() - auto_login_ms >= AUTO_LOGIN_RETRY_MS)) {
                auto_tried = true;
                auto_login_ms = millis();
                Serial.println("[game] logging in by itself (AUTO_LOGIN)");
                login();
            }
#endif
            break;
    }
#if LOGIN_FOB
    read_fob();
#else
    read_button();
#endif
    read_frames();
    draw();
}
