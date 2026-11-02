// BoxLink: the box's side of boxlink (docs/BOXLINK.md), for Arduino boxes.
//
// A box sends its own data as bytes on its own schemas (32-255); the proxy
// forwards them unchanged, and the hub's config says how to decode them. It
// also reports the game's events (win, fail, hint, end), which the proxy sends
// for whichever team logged in.
//
//   #include <BoxLink.h>
//   BoxLink proxy(Serial1);  // not "link": that is a POSIX function
//
//   struct __attribute__((packed)) Reading {  // the hub: struct = "<hfB"
//       int16_t temp_cC;
//       float humidity;
//       uint8_t flags;
//   };
//
//   void setup() { Serial1.begin(115200); proxy.begin("vault", 1); }
//   void loop() {
//       proxy.poll();
//       Reading r = {2215, 48.5f, 0x01};
//       proxy.send(32, r);                         // a struct, as its bytes
//       proxy.send(33, BoxLink::Payload{buf, n});  // or any bytes and a length
//       if (solved) proxy.trigger(BoxLink::WIN);
//   }
#pragma once
#include <Arduino.h>

#include "boxlink_frame.h"

class BoxLink {
public:
    // The game's events (OP_TRIGGER)
    static constexpr uint8_t WIN = boxlink::TRIGGER_WIN;
    static constexpr uint8_t FAIL = boxlink::TRIGGER_FAIL;
    static constexpr uint8_t HINT = boxlink::TRIGGER_HINT;
    static constexpr uint8_t END = boxlink::TRIGGER_END;

    // What send() and trigger() return: OK, a REFUSED_* code from
    // boxlink_frame.h (NO_SESSION, BUSY, OFFLINE...), or NO_ANSWER.
    static constexpr int OK = 0;
    static constexpr int NO_ANSWER = -1;

    // Bytes and their length: what a box sends.
    struct Payload {
        const void *data;
        size_t length;
    };

    // The game's verdict, passed on by the proxy (GAME.md section 6 result codes).
    struct Verdict {
        uint8_t result;  // 0 solved, 3 locked, 6 go, 7 busy, 9 hint granted...
        uint16_t arg;
        uint8_t solved, required;
    };

    explicit BoxLink(Stream &port) : port_(port) {}

    // Says HELLO, so the proxy knows the box and tells it the link and session.
    // Returns OK, or NO_ANSWER if no proxy answered (check the wiring).
    int begin(const char *box_type, uint16_t box_version);

    // Call often (every loop()): reads what the proxy sent and calls the handlers.
    // When the box has sent nothing for 5 s, it also sends a PING, which the
    // proxy answers with its STATUS: that keeps in_session() and link_up() right
    // even if a frame from the proxy was lost.
    void poll();

    // Asks the proxy for its STATUS and waits for the answer: OK, or NO_ANSWER.
    int ping();

    // Sends the box's own data on `schema` (32-255), at most 200 bytes, and
    // waits for the proxy's answer (up to about 6 s). OK once it's forwarded.
    int send(uint8_t schema, Payload p);
    template <class T>
    int send(uint8_t schema, const T &value) {
        static_assert(sizeof(T) <= boxlink::MAX_PAYLOAD, "at most 200 bytes");
        return send(schema, Payload{&value, sizeof(T)});
    }

    // Reports a game event: WIN, FAIL, END, or HINT with the hint number (0:
    // the next one). Needs a session: someone logged in at the proxy. At most
    // one a second: the proxy refuses faster ones (REFUSED_BUSY).
    int trigger(uint8_t what, uint8_t arg = 0);

    bool in_session() const { return session_ != 0; }
    bool link_up() const { return link_ == boxlink::LINK_UP; }

    // Handlers, called from poll(), and while begin(), send(), trigger() and
    // ping() wait: a VERDICT can arrive before the ACK of the trigger that
    // asked for it. Calling send(), trigger() or ping() from a handler then
    // returns REFUSED_BUSY at once; note what to do, and do it in loop().
    void on_session(void (*fn)(bool open)) { on_session_ = fn; }
    void on_verdict(void (*fn)(const Verdict &v)) { on_verdict_ = fn; }
    void on_data(void (*fn)(uint8_t schema, const uint8_t *data, size_t length)) { on_data_ = fn; }

private:
    int request(uint8_t type, uint8_t schema, const void *data, size_t length);
    void transmit(const boxlink::Frame &f);
    void handle(const boxlink::Frame &f);
    void set_session(uint16_t session);  // calls on_session if it opened or closed

    Stream &port_;
    boxlink::Decoder decoder_ = {};
    uint8_t seq_ = 0;
    uint16_t session_ = 0;
    uint8_t link_ = boxlink::LINK_DOWN;
    bool waiting_ = false;
    uint8_t waiting_seq_ = 0;
    int answer_ = NO_ANSWER;
    uint32_t last_tx_ms_ = 0;
    void (*on_session_)(bool) = nullptr;
    void (*on_verdict_)(const Verdict &) = nullptr;
    void (*on_data_)(uint8_t, const uint8_t *, size_t) = nullptr;
};
