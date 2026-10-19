#include "rlink.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <roclink.h>
#include <roclink_secure.h>

#include "config.h"

namespace rlink {

static Preferences store;
static WiFiUDP udp;
static bool udp_open = false;
static IPAddress gateway(GATEWAY_IP);
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

static bool transmit(const uint8_t *frame, int len) {
    if (!udp_open) return false;
    digitalWrite(PIN_LED, HIGH);
    udp.beginPacket(gateway, GATEWAY_PORT);
    udp.write(frame, (size_t)len);
    bool ok = udp.endPacket() == 1;
    digitalWrite(PIN_LED, LOW);
    return ok;
}

// Handles one received datagram: open, interpret, ACK downlink if asked.
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
        if (udp_open) {
            int size = udp.parsePacket();
            if (size > 0) {
                uint8_t frame[ROCLINK_FRAME_MAX_LEN];
                int n = udp.read(frame, sizeof(frame));
                if (n > 0) handle(frame, (size_t)n);
                continue;
            }
        }
        if (ms) delay(5);
    } while (millis() - start < ms);
}

bool begin(DownlinkHandler on_downlink) {
    handler = on_downlink;
    pinMode(PIN_LED, OUTPUT);

    static const uint8_t provisioned[16] = NODE_CHAIN_KEY;
    store.begin("roclink", false);
    uint8_t rec[20];
    uint32_t epoch = 0;
    if (store.getBytes("key", rec, sizeof(rec)) != sizeof(rec)) {
        store_key(provisioned, 0);  // first boot
        memcpy(rec + 4, provisioned, 16);
    } else {
        memcpy(&epoch, rec, 4);
    }
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

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[link] node %02X/%04X, epoch %lu, joining %s%s\n", ROCLINK_NET, ROCLINK_NODE,
                  (unsigned long)epoch, WIFI_SSID,
#ifdef NODE_KEY_IS_DEMO
                  " (DEMO key)"
#else
                  ""
#endif
    );
    return true;
}

bool wifi_up() { return WiFi.status() == WL_CONNECTED; }

void loop() {
    if (wifi_up() && !udp_open) {
        udp_open = udp.begin(GATEWAY_PORT) == 1;  // fixed port: the gateway answers here
        Serial.printf("[link] Wi-Fi up: %s, gateway %s:%d\n", WiFi.localIP().toString().c_str(),
                      gateway.toString().c_str(), GATEWAY_PORT);
    } else if (!wifi_up() && udp_open) {
        udp.stop();
        udp_open = false;
        Serial.println("[link] Wi-Fi down");
    }
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
        receive_for(ACK_WAIT_MS);
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
