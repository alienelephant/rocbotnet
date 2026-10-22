#include "BoxLink.h"

#include <string.h>

using namespace boxlink;

static constexpr uint32_t ANSWER_WAIT_MS = 3000;  // the proxy forwards a frame before answering it
static constexpr int ATTEMPTS = 2;

int BoxLink::begin(const char *box_type, uint16_t box_version) {
    // A random first seq: after a restart, the box's frames mustn't look like
    // retries of the ones it sent before (the proxy answers those again, unrepeated).
    seq_ = (uint8_t)random(256);
    uint8_t p[1 + 1 + 16 + 2];
    size_t tl = strnlen(box_type, 16), n = 0;
    p[n++] = VERSION;
    p[n++] = (uint8_t)tl;
    memcpy(p + n, box_type, tl);
    n += tl;
    p[n++] = (uint8_t)box_version;
    p[n++] = (uint8_t)(box_version >> 8);
    return request(CTRL, OP_HELLO, p, n);
}

void BoxLink::poll() {
    Frame f;
    while (port_.available())
        if (decoder_.feed((uint8_t)port_.read(), &f)) handle(f);
}

int BoxLink::send(uint8_t schema, Payload p) {
    if (schema < FIRST_BOX_SCHEMA) return REFUSED_BAD_SCHEMA;
    if (p.length > MAX_PAYLOAD) return REFUSED_TOO_LONG;
    return request(DATA, schema, p.data, p.length);
}

int BoxLink::trigger(uint8_t what, uint8_t arg) {
    uint8_t p[2] = {what, arg};
    return request(CTRL, OP_TRIGGER, p, sizeof p);
}

// Stop-and-wait: one frame at a time, the same bytes again if no answer.
int BoxLink::request(uint8_t type, uint8_t schema, const void *data, size_t length) {
    Frame f = {type, FLAG_ACK_REQ, ++seq_, schema, (uint8_t)length, {}};
    memcpy(f.payload, data, length);
    waiting_ = true;
    waiting_seq_ = f.seq;
    answer_ = NO_ANSWER;
    for (int attempt = 0; attempt < ATTEMPTS && answer_ == NO_ANSWER; ++attempt) {
        transmit(f);
        uint32_t start = millis();
        while (answer_ == NO_ANSWER && millis() - start < ANSWER_WAIT_MS) {
            poll();
            delay(1);
        }
    }
    waiting_ = false;
    return answer_;
}

void BoxLink::transmit(const Frame &f) {
    uint8_t wire[MAX_WIRE];
    size_t n = encode(f, wire);
    port_.write((uint8_t)0x00);  // a delimiter first: the proxy drops any half-read frame
    port_.write(wire, n);
}

void BoxLink::handle(const Frame &f) {
    if (f.type == ACK) {
        if (waiting_ && f.seq == waiting_seq_) answer_ = f.length ? f.payload[0] : OK;
        return;
    }
    if (f.flags & FLAG_ACK_REQ) {  // the proxy doesn't ask for ACKs today; answer if it does
        Frame ack = {ACK, 0, f.seq, 0, 0, {}};
        transmit(ack);
    }
    if (f.type == DATA) {
        if (on_data_) on_data_(f.schema, f.payload, f.length);
        return;
    }
    const uint8_t *p = f.payload;
    switch (f.schema) {
        case OP_HELLO_ACK:  // ver, proxy_version u16, max_payload, link, session u16
            if (f.length < 7) return;
            link_ = p[4];
            if ((p[5] | p[6] << 8) != session_) {
                session_ = (uint16_t)(p[5] | p[6] << 8);
                if (on_session_) on_session_(session_ != 0);
            }
            return;
        case OP_STATUS:  // link, session u16, budget
            if (f.length < 3) return;
            link_ = p[0];
            if ((p[1] | p[2] << 8) != session_) {
                session_ = (uint16_t)(p[1] | p[2] << 8);
                if (on_session_) on_session_(session_ != 0);
            }
            return;
        case OP_SESSION_OPEN:  // session u16, lifetime_s u16, label
            if (f.length < 2) return;
            session_ = (uint16_t)(p[0] | p[1] << 8);
            if (on_session_) on_session_(true);
            return;
        case OP_SESSION_CLOSED:  // session u16, reason
            session_ = 0;
            if (on_session_) on_session_(false);
            return;
        case OP_VERDICT:  // result, arg u16, solved, required
            if (f.length < 5) return;
            if (on_verdict_) on_verdict_(Verdict{p[0], (uint16_t)(p[1] | p[2] << 8), p[3], p[4]});
            return;
        default:
            return;
    }
}
