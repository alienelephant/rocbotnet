// rfidprox, breadboard firmware for a Heltec WiFi LoRa 32 V3 or V4 (docs/BREADBOARD.md).
//
// Joins the rfgateway's Wi-Fi as roclink node 0x0301, says HELLO, keeps the
// link checked with PINGs, and sends an AUTH_REQ for every card tapped on the
// RC522 (BACKEND.md section 1). With no backend yet, the AUTH_REQ is ACKed by
// the gateway and published on MQTT, but no GRANT comes back: the display says
// so. When rfidauth exists, a GRANT shows the user's label.
//
// Not here yet: the session state machine, boxlink, LoRa (PLAN_WIFI_EXAMPLE.md).
#include <Arduino.h>
#include <MFRC522DriverPinSimple.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522v2.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <Wire.h>

#include "config.h"
#include "rlink.h"
#include "messages.h"

// --- hardware
static SPIClass rfid_spi(HSPI);  // its own bus: the LoRa radio has the other one
static MFRC522DriverPinSimple rfid_ss(PIN_RC522_SS);
static MFRC522DriverSPI rfid_driver{rfid_ss, rfid_spi, SPISettings(4000000, MSBFIRST, SPI_MODE0)};
static MFRC522 rfid{rfid_driver};
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, PIN_OLED_RST, PIN_OLED_SCL, PIN_OLED_SDA);
static bool reader_ok;

// --- state
enum class State { joining, hello, idle, authorizing };
static State state = State::joining;
static uint32_t last_ping_ms, auth_sent_ms, last_card_ms;
static uint8_t last_uid[10], last_uid_len, req_id;
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
    if (!rlink::wifi_up()) snprintf(l2, sizeof(l2), "Wi-Fi: joining");
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
    oled.drawStr(0, 62, reader_ok ? detail : "RC522 not found");
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

#include <MFRC522Debug.h>

// RC522 diagnostics (RC522_DEBUG in config.h): what the reader sees on every
// poll, not just successful reads, summarised every RC522_REPORT_MS.
static uint32_t dbg_polls, dbg_silent, dbg_answers, dbg_other, dbg_select_fail, dbg_last_report;
static uint8_t dbg_last_version;

static uint8_t reg(MFRC522Constants::PCD_Register r) { return rfid_driver.PCD_ReadRegister(r); }

static const char *version_name(uint8_t v) {
    switch (v) {
        case 0x91: return "MFRC522 v1.0";
        case 0x92: return "MFRC522 v2.0";
        case 0x88: return "FM17522 clone";
        case 0x12: return "counterfeit chip";
        case 0xB2: return "FM17522 clone";
        case 0x00: case 0xFF: return "NO ANSWER on SPI";
        default: return "unknown chip";
    }
}

static void reader_begin() {
    Serial.printf("[rc522] pins: SDA/SS GPIO%d, SCK GPIO%d, MOSI GPIO%d, MISO GPIO%d, RST GPIO%d\n",
                  PIN_RC522_SS, PIN_RC522_SCK, PIN_RC522_MOSI, PIN_RC522_MISO, PIN_RC522_RST);
    pinMode(PIN_RC522_RST, OUTPUT);  // hardware reset: low, then high
    digitalWrite(PIN_RC522_RST, LOW);
    delay(10);
    digitalWrite(PIN_RC522_RST, HIGH);
    delay(50);
    rfid_spi.begin(PIN_RC522_SCK, PIN_RC522_MISO, PIN_RC522_MOSI, PIN_RC522_SS);
    rfid.PCD_Init();
    uint8_t v = reg(MFRC522Constants::VersionReg);
    dbg_last_version = v;
    reader_ok = v != 0x00 && v != 0xFF;
    Serial.printf("[rc522] version register 0x%02X: %s\n", v, version_name(v));
    if (!reader_ok) {
        Serial.println("[rc522] NOT FOUND: check the wiring by header pin, and 3.3 V (docs/BREADBOARD.md)");
        return;
    }
#if RC522_DEBUG
    // The chip's own self-test (it resets the chip, so initialise again after).
    // Genuine NXP chips pass; many clones fail it and still read cards fine.
    bool self_test = rfid.PCD_PerformSelfTest();
    Serial.printf("[rc522] self-test: %s\n",
                  self_test ? "PASS" : "FAIL (normal for many clones; not conclusive on its own)");
    rfid.PCD_Init();
#endif
    rfid.PCD_SetAntennaGain(MFRC522Constants::RxGain_max);  // 48 dB: the best range
    rfid.PCD_AntennaOn();
    uint8_t tx = reg(MFRC522Constants::TxControlReg);
    Serial.printf("[rc522] antenna %s (TxControlReg 0x%02X), receiver gain 0x%02X (max 0x70)\n",
                  (tx & 0x03) == 0x03 ? "ON" : "OFF", tx, rfid.PCD_GetAntennaGain());
}

// One poll: asks any card in the field to answer (REQA). Returns true with a
// card's UID, ATQA and SAK. Each tap counts once: a read card is halted until
// it leaves the field.
static bool read_card(uint8_t *uid, uint8_t *uid_len, uint16_t *atqa, uint8_t *sak) {
    byte buf[2];
    byte size = sizeof(buf);
    dbg_polls++;
    auto st = rfid.PICC_RequestA(buf, &size);
    if (st == MFRC522Constants::STATUS_TIMEOUT) {  // nothing answered: no card in the field
        dbg_silent++;
        return false;
    }
    if (st != MFRC522Constants::STATUS_OK && st != MFRC522Constants::STATUS_COLLISION) {
        // Something answered, but garbled: a card at the edge of the field, a weak
        // antenna, or noise. Worth seeing every time.
        dbg_other++;
#if RC522_DEBUG
        Serial.printf("[rc522] REQA answered with an error: %s\n",
                      (const char *)MFRC522Debug::GetStatusCodeName(st));
#endif
        return false;
    }
    dbg_answers++;
    *atqa = (uint16_t)(buf[0] | buf[1] << 8);
#if RC522_DEBUG
    Serial.printf("[rc522] a card answered REQA: ATQA %04X%s; reading its UID\n", *atqa,
                  st == MFRC522Constants::STATUS_COLLISION ? " (several cards: collision)" : "");
#endif
    auto sel = rfid.PICC_Select(&rfid.uid, 0);
    if (sel != MFRC522Constants::STATUS_OK) {
        dbg_select_fail++;
#if RC522_DEBUG
        Serial.printf("[rc522] UID read (anticollision/select) failed: %s. Hold the card still and flat.\n",
                      (const char *)MFRC522Debug::GetStatusCodeName(sel));
#endif
        return false;
    }
    *sak = rfid.uid.sak;
    *uid_len = rfid.uid.size;
    memcpy(uid, rfid.uid.uidByte, rfid.uid.size);
    rfid.PICC_HaltA();
    return true;
}

// Every RC522_REPORT_MS: what the reader saw, and whether it still answers on SPI.
static void reader_report() {
#if RC522_DEBUG
    if (millis() - dbg_last_report < RC522_REPORT_MS) return;
    dbg_last_report = millis();
    uint8_t v = reg(MFRC522Constants::VersionReg);
    uint8_t tx = reg(MFRC522Constants::TxControlReg);
    Serial.printf("[rc522] last %lus: %lu polls, %lu silent, %lu card answers, %lu UID failures, "
                  "%lu errors | version 0x%02X%s, antenna %s | link: %s\n",
                  (unsigned long)(RC522_REPORT_MS / 1000), (unsigned long)dbg_polls,
                  (unsigned long)dbg_silent, (unsigned long)dbg_answers,
                  (unsigned long)dbg_select_fail, (unsigned long)dbg_other, v,
                  v != dbg_last_version ? " (CHANGED: loose wire?)" : "",
                  (tx & 0x03) == 0x03 ? "on" : "OFF",
                  state == State::joining ? "joining Wi-Fi" : state == State::hello ? "waiting for HELLO ACK"
                  : state == State::idle ? "ready" : "waiting for GRANT");
    if (!reader_ok && v != 0x00 && v != 0xFF) {
        Serial.println("[rc522] the reader answers now: initialising it again");
        reader_begin();
    }
    if (reader_ok && dbg_polls == 0) Serial.println("[rc522] NOT POLLED: the loop isn't reaching the reader");
    dbg_last_version = v;
    dbg_polls = dbg_silent = dbg_answers = dbg_other = dbg_select_fail = 0;
#endif
}

static void on_card() {
    uint8_t uid[10], uid_len, sak;
    uint16_t atqa;
    if (!read_card(uid, &uid_len, &atqa, &sak)) return;
    bool same = uid_len == last_uid_len && !memcmp(uid, last_uid, uid_len);
    if (same && millis() - last_card_ms < SAME_CARD_MS) return;
    last_card_ms = millis();
    memcpy(last_uid, uid, uid_len);
    last_uid_len = uid_len;

    char hex[21];
    for (uint8_t i = 0; i < uid_len; ++i) snprintf(hex + 2 * i, 3, "%02X", uid[i]);
    Serial.printf("[rc522] card %s (ATQA %04X, SAK %02X)\n", hex, atqa, sak);
    beep(2000, 60);

    if (state == State::authorizing) return;  // one request at a time
    if (state != State::idle) {
        show("Offline", hex);
        return;
    }
    uint8_t payload[24];
    size_t n = msg::auth_req(payload, ++req_id, atqa, sak, uid, uid_len);
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
            if (rlink::wifi_up()) state = State::hello;
            break;
        case State::hello: {
            if (hello_retry_ms && millis() - hello_retry_ms < 3000) break;  // don't block the reader
            uint8_t p[24];
            size_t n = msg::hello(p, FW_VERSION, msg::RADIO_WIFI_UDP, "");
            if (rlink::send(msg::HELLO, p, n, true)) {
                Serial.println("[rfidprox] HELLO ACKed: ready");
                show("Tap a card", "");
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
            if (!rlink::wifi_up()) {
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
    if (reader_ok) on_card();
    reader_report();
    draw();
}
