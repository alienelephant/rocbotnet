#include "rlink.h"

#include <Arduino.h>
#include <Preferences.h>
#include <roclink.h>
#include <roclink_secure.h>

#include "config.h"
#include "transport.h"

namespace rlink {

static Preferences store;
static roclink_client_t node;
static roclink_sec_t sec;
static roclink_fcnt_t ctr;
static DownlinkHandler handler;
static Stats st;
static uint32_t last_epoch_ms;
static bool acked;  // the frame we're waiting on was ACKed

static void halt(const char *what) {
    Serial.printf("[link] FATAL: %s\n", what);
    while (true) delay(1000);
}

// The chain key and its epoch, one NVS entry: overwriting it erases the old
// key (SECURITY.md R4).
static void store_key(const uint8_t key[16], uint32_t epoch) {
    uint8_t rec[20];
    memcpy(rec, &epoch, 4);
    memcpy(rec + 4, key, 16);
    if (store.putBytes("key", rec, sizeof(rec)) != sizeof(rec)) halt("cannot store the key");
    roclink_crypto_wipe(rec, sizeof(rec));
}

// Seals a plain frame, persisting first whatever a power cut must not lose.
static int seal(uint8_t *frame, int len, size_t cap) {
    for (;;) {
        int n = roclink_sec_seal(&sec, &ctr, NULL, NULL, frame, (size_t)len, frame, cap);
        if (n == ROCLINK_E_PERSIST) {
            uint32_t v = roclink_fcnt_to_persist(&ctr);
            if (store.putULong("fcnt", v) != sizeof(uint32_t)) halt("cannot store the counter");
            roclink_fcnt_persisted(&ctr, v);
        } else if (n == ROCLINK_E_PERSIST_KEY) {
            uint8_t key[16];
            uint32_t epoch = 0;
            roclink_sec_key_pending(&sec, key, &epoch);
            store_key(key, epoch);
            roclink_crypto_wipe(key, sizeof(key));
            roclink_sec_key_persisted(&sec, epoch);
            Serial.printf("[link] key ratchet: epoch %lu, old key erased\n", (unsigned long)epoch);
        } else {
            return n;
        }
    }
}

static bool transmit(const uint8_t *frame, int len) { return transport::send(frame, (size_t)len); }

// Handles one received frame: open, interpret, ACK downlink if asked.
static void handle(uint8_t *frame, size_t len) {
    roclink_sec_info_t info;
    int n = roclink_sec_open(&sec, NULL, frame, len, &info);
    if (n < 0) {
        st.refused++;
        Serial.printf("[link] refused a %u-byte frame: %s\n", (unsigned)len, roclink_status_str(n));
        return;
    }
    roclink_rx_t rx;
    roclink_client_handle(&node, frame, (size_t)n, &rx);
    switch (rx.kind) {
        case ROCLINK_RX_ACK:
            acked = true;
            st.acked++;
            st.last_ack_ms = millis();
            break;
        case ROCLINK_RX_DATA:
            st.downlinks++;
            Serial.printf("[link] downlink seq %u schema %u, %u bytes\n", rx.hdr.seq, rx.hdr.schema,
                          (unsigned)rx.payload_len);
            if (handler) handler(rx.hdr.schema, rx.payload, rx.payload_len);
            break;
        case ROCLINK_RX_DUPLICATE:
            Serial.printf("[link] downlink seq %u again: ACKing, not delivering\n", rx.hdr.seq);
            break;
        case ROCLINK_RX_CONTROL:
            Serial.println("[link] PING from the gateway");
            break;
        case ROCLINK_RX_DROP:
            Serial.printf("[link] dropped: %s\n", roclink_status_str(rx.status));
            return;
    }
    if (rx.ack_requested) {  // our ACK is a node frame like any other: sealed
        uint8_t ack[ROCLINK_FRAME_MAX_LEN];
        int an = roclink_client_build_ack(&node, &rx, ack, sizeof(ack));
        if (an > 0) an = seal(ack, an, sizeof(ack));
        if (an > 0) transmit(ack, an);
    }
}

static void receive_for(uint32_t ms) {
    uint32_t start = millis();
    do {
        uint8_t frame[ROCLINK_FRAME_MAX_LEN];
        int n = transport::receive(frame, sizeof(frame));
        if (n > 0) {
            handle(frame, (size_t)n);
            continue;
        }
        if (ms) delay(5);
    } while (millis() - start < ms);
}

bool begin(DownlinkHandler on_downlink) {
    handler = on_downlink;
#ifdef PIN_TX_LED
    pinMode(PIN_TX_LED, OUTPUT);
#endif

    static const uint8_t provisioned[16] = NODE_CHAIN_KEY;
    store.begin("roclink", false);
    uint8_t rec[20];
    uint32_t epoch = 0;
    // The stored key is the one the board first booted with. It belongs to the
    // node it was then: flashed as another node, the board starts that node's
    // key afresh, or it would sign as the new address with the old one's key
    // (the gateway refuses that: "auth"). The counter starts afresh too, for a
    // new node. Firmware before this stored no identity: then a key still at
    // epoch 0 that isn't this build's is taken for another node's, or an old
    // key, and replaced, keeping the counter.
    const uint32_t id = (uint32_t)ROCLINK_NET << 16 | ROCLINK_NODE;
    const uint32_t stored_id = store.getULong("id", 0);
    bool have = store.getBytes("key", rec, sizeof(rec)) == sizeof(rec);
    if (have) memcpy(&epoch, rec, 4);
    if (have && stored_id && stored_id != id) {
        Serial.printf("[link] the stored key is node %02lX/%04lX's: starting node %02X/%04X afresh\n",
                      (unsigned long)(stored_id >> 16), (unsigned long)(stored_id & 0xFFFF), ROCLINK_NET,
                      ROCLINK_NODE);
        store.remove("fcnt");
        have = false;
    } else if (have && epoch == 0 && memcmp(rec + 4, provisioned, 16) != 0) {
        Serial.println("[link] the stored key isn't this build's (another node's, or an old one): replacing it");
        have = false;
    }
    if (!have) {
        store_key(provisioned, 0);  // first boot, or a new identity
        memcpy(rec + 4, provisioned, 16);
        epoch = 0;
    }
    if (stored_id != id) store.putULong("id", id);
    roclink_sec_config_t sc;
    roclink_sec_config_default(&sc);  // enc, 8-byte tags
    sc.net_id = ROCLINK_NET;
    sc.addr = ROCLINK_NODE;
    sc.always_listening = true;  // mains-powered: gateway frames accepted at any time
    if (roclink_sec_init_node_at(&sec, &sc, rec + 4, 0, epoch) != ROCLINK_OK) {
        roclink_crypto_wipe(rec, sizeof(rec));
        Serial.println("[link] security setup failed (AES self-test?)");
        return false;
    }
    roclink_crypto_wipe(rec, sizeof(rec));
    roclink_fcnt_init(&ctr, store.getULong("fcnt", 0), 0);  // continue past the reservation

    roclink_client_config_t cfg = {ROCLINK_NET, ROCLINK_NODE, ROCLINK_ADDR_ANY_GATEWAY};
    roclink_client_init(&node, &cfg, (uint8_t)esp_random());
    last_epoch_ms = millis();

    Serial.printf("[link] node %02X/%04X, epoch %lu%s\n", ROCLINK_NET, ROCLINK_NODE, (unsigned long)epoch,
#ifdef NODE_KEY_IS_DEMO
                  " (DEMO key)"
#else
                  ""
#endif
    );
    return transport::begin();
}

bool ready() { return transport::ready(); }

const char *describe() { return transport::describe(); }

void loop() {
    transport::loop();
    if (millis() - last_epoch_ms >= EPOCH_MS) {  // the next frame ratchets the key
        roclink_fcnt_next_epoch(&ctr);
        last_epoch_ms += EPOCH_MS;
    }
    receive_for(0);
}

static bool deliver(uint8_t *frame, int len, bool ack, const char *what) {
    if (len < 0) {
        Serial.printf("[link] cannot build %s: %s\n", what, roclink_status_str(len));
        return false;
    }
    st.sent++;
    if (!ack) return transmit(frame, len);
    acked = false;
    for (int attempt = 0; attempt <= RETRIES && !acked; ++attempt) {
        transmit(frame, len);  // a retry is the identical sealed bytes
        receive_for(transport::ack_wait_ms());
        if (!acked && attempt < RETRIES) Serial.printf("[link] %s: no ACK, retrying\n", what);
    }
    if (!acked) {
        st.unacked++;
        roclink_client_cancel_ack(&node);
    }
    return acked;
}

bool send(uint8_t schema, const uint8_t *payload, size_t len, bool ack) {
    uint8_t frame[ROCLINK_FRAME_MAX_LEN];
    int n = roclink_client_build_data(&node, schema, payload, len,
                                      ack ? ROCLINK_SEND_ACK : ROCLINK_SEND_DEFAULT, frame,
                                      sizeof(frame));
    if (n > 0) n = seal(frame, n, sizeof(frame));
    char what[24];
    snprintf(what, sizeof(what), "schema %u", schema);
    return deliver(frame, n, ack, what);
}

bool ping() {
    uint8_t frame[ROCLINK_FRAME_MAX_LEN];
    int n = roclink_client_build_ping(&node, ROCLINK_SEND_ACK, frame, sizeof(frame));
    if (n > 0) n = seal(frame, n, sizeof(frame));
    return deliver(frame, n, true, "PING");
}

const Stats &stats() {
    st.fcnt = ctr.next;
    return st;
}

}  // namespace rlink
