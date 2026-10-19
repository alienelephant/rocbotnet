/* Client (node) tests. Gateway frames are built by hand with roclink_encode(),
 * not by the gateway code, so the node side is tested on its own. */
#include "roclink.h"

#include "check.h"

#define NET 0x42
#define NODE 0x1234
#define GW 0xFF01

static roclink_client_t make_client(uint16_t gateway_id) {
    roclink_client_t c;
    roclink_client_config_t cfg;
    cfg.net_id = NET;
    cfg.node_id = NODE;
    cfg.gateway_id = gateway_id;
    CHECK_EQ(roclink_client_init(&c, &cfg, 7), ROCLINK_OK);
    return c;
}

/* A frame as a gateway would send it. */
static size_t gw_frame(uint8_t *out, uint8_t type, uint8_t flags, uint16_t src, uint16_t dst,
                       uint8_t seq, uint8_t schema, const uint8_t *data, size_t len) {
    roclink_header_t h;
    int n;
    h.type = type;
    h.flags = flags;
    h.net_id = NET;
    h.src = src;
    h.dst = dst;
    h.seq = seq;
    h.schema = schema;
    h.relay = 0;
    n = roclink_encode(&h, data, len, out, ROCLINK_FRAME_MAX_LEN);
    CHECK(n > 0);
    return n > 0 ? (size_t)n : 0u;
}

static void test_init_rejects(void) {
    roclink_client_t c;
    roclink_client_config_t cfg;
    cfg.net_id = NET;
    cfg.gateway_id = ROCLINK_ADDR_ANY_GATEWAY;
    cfg.node_id = ROCLINK_ADDR_ANY_GATEWAY;
    CHECK_EQ(roclink_client_init(&c, &cfg, 0), ROCLINK_E_ARG);
    cfg.node_id = 0xFF05; /* gateway range */
    CHECK_EQ(roclink_client_init(&c, &cfg, 0), ROCLINK_E_ARG);
    cfg.node_id = NODE;
    cfg.gateway_id = 0x0042; /* a node address is not a gateway */
    CHECK_EQ(roclink_client_init(&c, &cfg, 0), ROCLINK_E_ARG);
}

static void test_build_data(void) {
    roclink_client_t c = make_client(ROCLINK_ADDR_ANY_GATEWAY);
    const uint8_t hi[] = {'h', 'i'};
    const uint8_t v1[] = {0x20, 0x03, 0x42, 0x34, 0x12, 0x00, 0x00, 0x07, 0x01, 0x68, 0x69};
    uint8_t out[32];
    int n;

    n = roclink_client_build_data(&c, 1, hi, sizeof(hi), ROCLINK_SEND_ACK, out, sizeof(out));
    CHECK_BYTES(out, n, v1); /* golden vector 1 */
    CHECK(roclink_client_awaiting_ack(&c));
    CHECK_EQ(roclink_client_next_seq(&c), 8);

    n = roclink_client_build_data(&c, 1, hi, sizeof(hi), ROCLINK_SEND_LISTEN, out, sizeof(out));
    CHECK(n > 0);
    CHECK_EQ(out[1], ROCLINK_FLAG_LISTEN);
    CHECK_EQ(out[7], 8);
    CHECK(!roclink_client_awaiting_ack(&c)); /* a new send replaces the old wait */

    CHECK_EQ(roclink_client_build_data(&c, ROCLINK_SCHEMA_CONTROL, hi, 2, 0, out, sizeof(out)),
             ROCLINK_E_ARG);
    CHECK_EQ(roclink_client_build_data(&c, 1, hi, 2, 0x80, out, sizeof(out)), ROCLINK_E_ARG);
    CHECK_EQ(roclink_client_build_data(&c, 1, hi, 2, 0, out, 10), ROCLINK_E_NOSPACE);
    CHECK_EQ(roclink_client_next_seq(&c), 9); /* failures do not consume a seq */
}

static void test_ack_matching(void) {
    roclink_client_t c = make_client(ROCLINK_ADDR_ANY_GATEWAY);
    roclink_rx_t rx;
    uint8_t out[32];
    uint8_t f[ROCLINK_FRAME_MAX_LEN];
    size_t n;

    CHECK(roclink_client_build_data(&c, 1, NULL, 0, ROCLINK_SEND_ACK, out, sizeof(out)) > 0);

    n = gw_frame(f, ROCLINK_TYPE_ACK, ROCLINK_FLAG_MORE | ROCLINK_FLAG_DL_QUEUED, GW, NODE, 6, 0,
                 NULL, 0); /* stale seq */
    roclink_client_handle(&c, f, n, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_DROP);
    CHECK_EQ(rx.status, ROCLINK_E_UNEXPECTED);
    CHECK(roclink_client_awaiting_ack(&c));
    CHECK(!rx.more); /* a dropped frame changes nothing (node rule 5) */
    CHECK(!roclink_client_downlink_queued(&c));

    n = gw_frame(f, ROCLINK_TYPE_ACK, ROCLINK_FLAG_MORE | ROCLINK_FLAG_DL_QUEUED, GW, NODE, 7, 0,
                 NULL, 0);
    roclink_client_handle(&c, f, n, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_ACK);
    CHECK(rx.more);
    CHECK(roclink_client_downlink_queued(&c));
    CHECK(!roclink_client_awaiting_ack(&c));

    roclink_client_handle(&c, f, n, &rx); /* a second copy is no longer expected */
    CHECK_EQ(rx.kind, ROCLINK_RX_DROP);
    CHECK_EQ(rx.status, ROCLINK_E_UNEXPECTED);
}

static void test_downlink_and_duplicates(void) {
    roclink_client_t c = make_client(ROCLINK_ADDR_ANY_GATEWAY);
    roclink_rx_t rx;
    uint8_t f[ROCLINK_FRAME_MAX_LEN];
    uint8_t ack[16];
    const uint8_t v4[] = {0x24, 0x00, 0x42, 0x34, 0x12, 0x01, 0xFF, 0x90, 0x00};
    const uint8_t cmd[] = {0x3C, 0x00};
    const uint8_t ping[] = {ROCLINK_CONTROL_PING};
    const uint8_t unknown[] = {0x02};
    size_t n;
    int an;

    n = gw_frame(f, ROCLINK_TYPE_DATA, ROCLINK_FLAG_ACK_REQ, GW, NODE, 0x90, 10, cmd, sizeof(cmd));
    roclink_client_handle(&c, f, n, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_DATA);
    CHECK_EQ(rx.hdr.schema, 10);
    CHECK_EQ(rx.payload_len, 2);
    CHECK(rx.payload == f + ROCLINK_HEADER_LEN);
    CHECK(rx.ack_requested);
    an = roclink_client_build_ack(&c, &rx, ack, sizeof(ack));
    CHECK_BYTES(ack, an, v4); /* golden vector 4 */

    roclink_client_handle(&c, f, n, &rx); /* gateway re-offered: our ACK was lost */
    CHECK_EQ(rx.kind, ROCLINK_RX_DUPLICATE);
    CHECK(rx.ack_requested); /* ACK it again, don't deliver it again */

    /* The hub re-offered it through another gateway with the same seq: still a
     * duplicate, so the command can't run twice (node rule 4). */
    n = gw_frame(f, ROCLINK_TYPE_DATA, 0, 0xFF02, NODE, 0x90, 10, cmd, sizeof(cmd));
    roclink_client_handle(&c, f, n, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_DUPLICATE);
    CHECK(!rx.ack_requested);
    CHECK_EQ(roclink_client_build_ack(&c, &rx, ack, sizeof(ack)), ROCLINK_E_ARG);
    n = gw_frame(f, ROCLINK_TYPE_DATA, 0, 0xFF02, NODE, 0x91, 10, cmd, sizeof(cmd));
    roclink_client_handle(&c, f, n, &rx); /* a new seq: new downlink */
    CHECK_EQ(rx.kind, ROCLINK_RX_DATA);

    n = gw_frame(f, ROCLINK_TYPE_DATA, ROCLINK_FLAG_ACK_REQ, 0xFF02, NODE, 0x92,
                 ROCLINK_SCHEMA_CONTROL, ping, sizeof(ping));
    roclink_client_handle(&c, f, n, &rx); /* "are you there?" from a gateway */
    CHECK_EQ(rx.kind, ROCLINK_RX_CONTROL);
    CHECK(rx.ack_requested);

    /* An unknown control operation, or none, is malformed: dropped, not ACKed. */
    n = gw_frame(f, ROCLINK_TYPE_DATA, ROCLINK_FLAG_ACK_REQ, 0xFF02, NODE, 0x93,
                 ROCLINK_SCHEMA_CONTROL, unknown, sizeof(unknown));
    roclink_client_handle(&c, f, n, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_DROP);
    CHECK_EQ(rx.status, ROCLINK_E_MALFORMED);
    CHECK(!rx.ack_requested);
    n = gw_frame(f, ROCLINK_TYPE_DATA, 0, 0xFF02, NODE, 0x94, ROCLINK_SCHEMA_CONTROL, NULL, 0);
    roclink_client_handle(&c, f, n, &rx);
    CHECK_EQ(rx.status, ROCLINK_E_MALFORMED);
    n = gw_frame(f, ROCLINK_TYPE_DATA, 0, 0xFF02, NODE, 0x93, 10, cmd, sizeof(cmd));
    roclink_client_handle(&c, f, n, &rx); /* the dropped frames left no state behind */
    CHECK_EQ(rx.kind, ROCLINK_RX_DATA);
}

static void test_build_ping(void) {
    roclink_client_t c = make_client(ROCLINK_ADDR_ANY_GATEWAY);
    /* PROTOCOL.md 7.1: schema 0, payload = the PING operation; 10 bytes. */
    const uint8_t check[] = {0x20, 0x03, 0x42, 0x34, 0x12, 0x00, 0x00, 0x07, 0x00, 0x01};
    uint8_t out[16];
    int n;

    n = roclink_client_build_ping(&c, ROCLINK_SEND_ACK, out, sizeof(out)); /* a link check */
    CHECK_BYTES(out, n, check);
    CHECK(roclink_client_awaiting_ack(&c));
    CHECK_EQ(roclink_client_next_seq(&c), 8);

    n = roclink_client_build_ping(&c, ROCLINK_SEND_DEFAULT, out, sizeof(out)); /* a heartbeat */
    CHECK_EQ(n, 10);
    CHECK_EQ(out[1], 0);
    CHECK(!roclink_client_awaiting_ack(&c));

    n = roclink_client_build_ping(&c, ROCLINK_SEND_LISTEN, out, sizeof(out)); /* a poll */
    CHECK_EQ(out[1], ROCLINK_FLAG_LISTEN);

    CHECK_EQ(roclink_client_build_ping(NULL, 0, out, sizeof(out)), ROCLINK_E_ARG);
    CHECK_EQ(roclink_client_build_ping(&c, 0x80, out, sizeof(out)), ROCLINK_E_ARG);
    CHECK_EQ(roclink_client_build_ping(&c, 0, out, 9), ROCLINK_E_NOSPACE);
    CHECK_EQ(roclink_client_next_seq(&c), 10); /* failures do not consume a seq */
}

static void test_filtering(void) {
    roclink_client_t c = make_client(GW); /* bound to one gateway */
    roclink_rx_t rx;
    uint8_t f[ROCLINK_FRAME_MAX_LEN];
    size_t n;

    n = gw_frame(f, ROCLINK_TYPE_DATA, 0, 0xFF02, NODE, 1, 5, NULL, 0); /* other gateway */
    roclink_client_handle(&c, f, n, &rx);
    CHECK_EQ(rx.status, ROCLINK_E_ADDR);

    n = gw_frame(f, ROCLINK_TYPE_DATA, 0, 0x0042, NODE, 1, 5, NULL, 0); /* from a node */
    roclink_client_handle(&c, f, n, &rx);
    CHECK_EQ(rx.status, ROCLINK_E_ADDR);

    n = gw_frame(f, ROCLINK_TYPE_DATA, 0, GW, 0x4321, 1, 5, NULL, 0); /* for another node */
    roclink_client_handle(&c, f, n, &rx);
    CHECK_EQ(rx.status, ROCLINK_E_ADDR);

    n = gw_frame(f, ROCLINK_TYPE_DATA, 0, GW, ROCLINK_ADDR_BROADCAST, 1, 5, NULL, 0);
    roclink_client_handle(&c, f, n, &rx); /* broadcast is reserved in v1 */
    CHECK_EQ(rx.status, ROCLINK_E_ADDR);

    n = gw_frame(f, ROCLINK_TYPE_DATA, 0, GW, NODE, 1, 5, NULL, 0);
    f[2] = NET + 1;
    roclink_client_handle(&c, f, n, &rx);
    CHECK_EQ(rx.status, ROCLINK_E_NET);

    roclink_client_handle(&c, f, 4, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_DROP);
    CHECK_EQ(rx.status, ROCLINK_E_SHORT);

    /* An ACK with a payload byte is malformed, even while an ACK is awaited. */
    CHECK(roclink_client_build_data(&c, 1, NULL, 0, ROCLINK_SEND_ACK, f, sizeof(f)) > 0);
    n = gw_frame(f, ROCLINK_TYPE_ACK, 0, GW, NODE, c.ack_seq, 0, NULL, 0);
    f[n++] = 0xAA;
    roclink_client_handle(&c, f, n, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_DROP);
    CHECK_EQ(rx.status, ROCLINK_E_MALFORMED);
    CHECK(roclink_client_awaiting_ack(&c));
    roclink_client_cancel_ack(&c);

    n = gw_frame(f, ROCLINK_TYPE_DATA, 0, GW, NODE, 1, 5, NULL, 0);
    roclink_client_handle(&c, f, n, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_DATA);
}

int main(void) {
    RUN(test_init_rejects);
    RUN(test_build_data);
    RUN(test_build_ping);
    RUN(test_ack_matching);
    RUN(test_downlink_and_duplicates);
    RUN(test_filtering);
    return REPORT();
}
