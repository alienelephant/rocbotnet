// rfidprox, breadboard firmware for a Heltec WiFi LoRa 32 V3 or V4 (docs/BREADBOARD.md).
//
// Joins the rfgateway's Wi-Fi as roclink node 0x0301, says HELLO, keeps the
// link checked with PINGs, and sends an AUTH_REQ for every 125 kHz tag held to
// the RDM6300 (BACKEND.md section 1). With no backend yet, the AUTH_REQ is ACKed by
// the gateway and published on MQTT, but no GRANT comes back: the display says
// so. When rfidauth exists, a GRANT shows the user's label.
//
// Not here yet: the session state machine, boxlink, LoRa (PLAN_WIFI_EXAMPLE.md).
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

#include "config.h"
#include "rlink.h"
#include "messages.h"

// --- hardware
static HardwareSerial &reader = Serial2;  // UART2: UART1 is the box's (gamebox.cpp)
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, PIN_OLED_RST, PIN_OLED_SCL, PIN_OLED_SDA);

// --- state
enum class State { joining, hello, idle, authorizing };
static State state = State::joining;
static uint32_t last_ping_ms, auth_sent_ms, last_card_ms;
static uint8_t last_tag[5], req_id;
static char headline[22] = "Starting";
static char detail[22] = "";

// ------------------------------------------------------------------- display

static void beep(int freq, int ms) { tone(PIN_BUZZER, freq, ms); }

static void show(const char *line1, const char *line2 = "") {
    snprintf(headline, sizeof(headline), "%s", line1);
    snprintf(detail, sizeof(detail), "%s", line2);
}

static void draw() {
    static uint32_t last_draw;
    if (millis() - last_draw < 250) return;
    last_draw = millis();
    const rlink::Stats &s = rlink::stats();
    char l1[24], l2[24], l3[24];
    snprintf(l1, sizeof(l1), "rfidprox %02X/%04X", ROCLINK_NET, ROCLINK_NODE);
    if (!rlink::ready()) snprintf(l2, sizeof(l2), "Wi-Fi: joining");
    else if (!s.last_ack_ms) snprintf(l2, sizeof(l2), "link: no ACK yet");
    else snprintf(l2, sizeof(l2), "link: ACK %lus ago", (unsigned long)((millis() - s.last_ack_ms) / 1000));
    snprintf(l3, sizeof(l3), "tx %lu ack %lu dl %lu", (unsigned long)s.sent, (unsigned long)s.acked,
             (unsigned long)s.downlinks);
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

// ------------------------------------------------------------------ downlink

static void on_downlink(uint8_t schema, const uint8_t *p, size_t len) {
    if (schema == msg::GRANT && len >= 12 && p[0] == msg::VERSION) {
        uint8_t result = p[2];
        uint16_t session = (uint16_t)(p[3] | p[4] << 8);
        size_t ll = p[11] < len - 12 ? p[11] : len - 12;
        char label[25] = "";
        memcpy(label, p + 12, ll < 24 ? ll : 24);
        Serial.printf("[auth] GRANT req %u: result %u, session %04X, \"%s\"\n", p[1], result, session,
                      label);
        if (state != State::authorizing || p[1] != req_id) return;  // a late answer
        state = State::idle;
        if (result == 0) {
            show(label[0] ? label : "Granted", "session open");
            beep(2600, 120);
        } else {
            char why[22];
            snprintf(why, sizeof(why), "refused (%u)", result);
            show(label[0] ? label : "Not allowed", why);
            beep(400, 300);
        }
        return;
    }
    Serial.printf("[link] downlink schema %u not handled yet\n", schema);
}

// -------------------------------------------------------------------- reader

// The RDM6300 reads 125 kHz EM4100 tags and sends each one it sees as a frame
// on its TX line, 9600 8N1, again and again while the tag stays in the field:
//   0x02 | 10 hex digits: the tag's 5 bytes | 2 hex digits: their XOR | 0x03
// It sends nothing without a tag, so there's no way to ask whether it's there.

// Diagnostics (RDM6300_DEBUG in config.h): what arrived on the reader's UART,
// summarised every RDM6300_REPORT_MS.
static uint32_t dbg_bytes, dbg_frames, dbg_bad, dbg_last_report;

static int hex_digit(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Checks a frame's 12 digits (between 0x02 and 0x03) and decodes the tag.
static bool decode_frame(const uint8_t *digits, uint8_t *tag) {
    uint8_t sum = 0;
    for (int i = 0; i < 6; ++i) {
        int hi = hex_digit(digits[2 * i]), lo = hex_digit(digits[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        uint8_t b = (uint8_t)(hi << 4 | lo);
        if (i < 5) {
            tag[i] = b;
            sum ^= b;
        } else if (b != sum) {
            return false;
        }
    }
    return true;
}

// The reader's TX idles high, so through the divider GPIO4 should sit at 3.3 V.
// A weak pull-down makes a line that isn't driven read low instead of floating.
static bool reader_line_high() { return digitalRead(PIN_RDM6300_RX) == HIGH; }

static const char *line_verdict(bool high) {
    return high ? "HIGH: the reader is powered and its TX reaches GPIO4"
                : "LOW: nothing drives it. Check the reader's 5 V and GND, which header pin is TX, and the "
                  "10 kOhm from TX to the divider (docs/BREADBOARD.md 6)";
}

static void reader_begin() {
    Serial.printf("[rdm6300] TX to GPIO%d (through the 5 V to 3.3 V divider), %d baud\n", PIN_RDM6300_RX,
                  RDM6300_BAUD);
    reader.begin(RDM6300_BAUD, SERIAL_8N1, PIN_RDM6300_RX, -1);
    gpio_pullup_dis((gpio_num_t)PIN_RDM6300_RX);  // the UART driver may have enabled one
    gpio_pulldown_en((gpio_num_t)PIN_RDM6300_RX);
    delay(5);
    Serial.printf("[rdm6300] TX line %s\n", line_verdict(reader_line_high()));
}

// Reads what's waiting on the UART. Returns true with a tag's 5 bytes once a
// whole frame with a good checksum has arrived.
static bool read_card(uint8_t *tag) {
    static uint8_t frame[12];
    static int pos = -1;  // -1: waiting for 0x02
    while (reader.available()) {
        uint8_t c = (uint8_t)reader.read();
        dbg_bytes++;
        if (c == 0x02) {  // a frame starts, even in the middle of a broken one
            pos = 0;
        } else if (pos < 0) {
            continue;  // between frames: some modules add CR LF
        } else if (pos < (int)sizeof(frame)) {
            frame[pos++] = c;
        } else {
            bool ok = c == 0x03 && decode_frame(frame, tag);
            pos = -1;
            if (ok) {
                dbg_frames++;
                return true;
            }
            dbg_bad++;
#if RDM6300_DEBUG
            Serial.printf("[rdm6300] bad frame: %.12s (end byte 0x%02X)\n", (const char *)frame, c);
#endif
        }
    }
    return false;
}

// Every RDM6300_REPORT_MS: what the reader sent or, with nothing, the TX line's level.
static void reader_report() {
#if RDM6300_DEBUG
    if (millis() - dbg_last_report < RDM6300_REPORT_MS) return;
    dbg_last_report = millis();
    if (!dbg_bytes) {
        Serial.printf("[rdm6300] last %lus: no bytes (normal without a 125 kHz tag); TX line %s\n",
                      (unsigned long)(RDM6300_REPORT_MS / 1000), line_verdict(reader_line_high()));
        return;
    }
    Serial.printf("[rdm6300] last %lus: %lu bytes, %lu tag frames, %lu bad frames | link: %s\n",
                  (unsigned long)(RDM6300_REPORT_MS / 1000), (unsigned long)dbg_bytes,
                  (unsigned long)dbg_frames, (unsigned long)dbg_bad,
                  state == State::joining ? "joining Wi-Fi" : state == State::hello ? "waiting for HELLO ACK"
                  : state == State::idle ? "ready" : "waiting for GRANT");
    dbg_bytes = dbg_frames = dbg_bad = 0;
#endif
}

static void on_card() {
    uint8_t tag[5];
    if (!read_card(tag)) return;
    // The reader repeats a tag while it's held: it counts once, until it has
    // been away for SAME_CARD_MS.
    bool same = !memcmp(tag, last_tag, sizeof(tag)) && millis() - last_card_ms < SAME_CARD_MS;
    last_card_ms = millis();
    if (same) return;
    memcpy(last_tag, tag, sizeof(tag));

    char hex[11];
    for (uint8_t i = 0; i < sizeof(tag); ++i) snprintf(hex + 2 * i, 3, "%02X", tag[i]);
    Serial.printf("[rdm6300] tag %s (EM4100: id %lu)\n", hex,
                  (unsigned long)((uint32_t)tag[1] << 24 | tag[2] << 16 | tag[3] << 8 | tag[4]));
    beep(2000, 60);

    if (state == State::authorizing) return;  // one request at a time
    if (state != State::idle) {
        show("Offline", hex);
        return;
    }
    uint8_t payload[24];
    // A 125 kHz tag has no ATQA or SAK (ISO 14443A's): both 0 (BACKEND.md section 1).
    size_t n = msg::auth_req(payload, ++req_id, 0, 0, tag, sizeof(tag));
    show("Checking...", hex);
    draw();
    if (rlink::send(msg::AUTH_REQ, payload, n, true)) {
        state = State::authorizing;
        auth_sent_ms = millis();
        Serial.printf("[auth] AUTH_REQ %u sent and ACKed; waiting for a GRANT\n", req_id);
    } else {
        show("No link", hex);
        beep(400, 300);
    }
}

// ------------------------------------------------------------------- arduino

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("\n[rfidprox] firmware %04X, node %02X/%04X\n", FW_VERSION, ROCLINK_NET, ROCLINK_NODE);

    pinMode(PIN_VEXT, OUTPUT);
    digitalWrite(PIN_VEXT, VEXT_ON);  // power the OLED
    delay(50);
    oled.begin();
    pinMode(PIN_BUZZER, OUTPUT);

    reader_begin();
    if (!rlink::begin(on_downlink)) show("Security failed", "see serial log");
    else show("Joining Wi-Fi", WIFI_SSID);
}

void loop() {
    static uint32_t hello_retry_ms;
    rlink::loop();

    switch (state) {
        case State::joining:
            if (rlink::ready()) state = State::hello;
            break;
        case State::hello: {
            if (hello_retry_ms && millis() - hello_retry_ms < 3000) break;  // don't block the reader
            uint8_t p[24];
            size_t n = msg::hello(p, FW_VERSION, msg::RADIO_WIFI_UDP, "");
            if (rlink::send(msg::HELLO, p, n, true)) {
                Serial.println("[rfidprox] HELLO ACKed: ready");
                show("Hold a tag", "");
                state = State::idle;
                last_ping_ms = millis();
                hello_retry_ms = 0;
            } else {
                Serial.println("[rfidprox] HELLO not ACKed: is roclink-gw reachable at the gateway address?");
                show("No gateway", "retrying");
                hello_retry_ms = millis();
            }
            break;
        }
        case State::idle:
        case State::authorizing:
            if (!rlink::ready()) {
                show("Wi-Fi lost", "rejoining");
                state = State::joining;
                break;
            }
            if (state == State::authorizing && millis() - auth_sent_ms >= AUTH_TIMEOUT_MS) {
                Serial.println("[auth] no GRANT: is rfidauth running? (expected on the breadboard)");
                show("No answer", "no backend yet");
                beep(400, 200);
                state = State::idle;
            }
            if (millis() - last_ping_ms >= HEARTBEAT_IDLE_MS) {
                last_ping_ms = millis();
                bool ok = rlink::ping();
                Serial.printf("[link] PING: %s\n", ok ? "ACKed" : "no ACK");
            }
            break;
    }
    // The reader runs whatever the link is doing: a card tapped while offline is
    // still read and shown (on_card() says "Offline"), just not sent.
    on_card();
    reader_report();
    draw();
}
