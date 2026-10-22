/* Server (gateway) tests. Node frames are built by hand with roclink_encode(),
 * not by the node code, so the gateway side is tested on its own. */
#include "roclink.h"

#include "check.h"

#define NET 0x42
#define NODE 0x1234
#define GW 0xFF01

static roclink_peer_t g_peers[4];

static roclink_server_t make_server(bool answer_any) {
    roclink_server_t s;
    roclink_server_config_t cfg;
    roclink_server_config_default(&cfg);
    cfg.net_id = NET;
    cfg.gateway_id = GW;
    cfg.answer_any = answer_any;
    cfg.dedupe_window_ms = 1000;
    CHECK_EQ(roclink_server_init(&s, &cfg, g_peers, 4, 1234), ROCLINK_OK);
    return s;
}

static size_t node_frame(uint8_t *out, uint8_t type, uint8_t flags, uint16_t src, uint16_t dst,
                         uint8_t seq, uint8_t schema) {
    roclink_header_t h;
    const uint8_t data[] = {1, 2, 3};
    int n;
    h.type = type;
    h.flags = flags;
    h.net_id = NET;
    h.src = src;
    h.dst = dst;
    h.seq = seq;
    h.schema = schema;
    h.relay = 0;
    n = roclink_encode(&h, data, type == ROCLINK_TYPE_DATA ? sizeof(data) : 0, out,
                       ROCLINK_FRAME_MAX_LEN);
    CHECK(n > 0);
    return n > 0 ? (size_t)n : 0u;
}

static void test_init_rejects(void) {
    roclink_server_t s;
    roclink_server_config_t cfg;
    roclink_server_config_default(&cfg);
    CHECK_EQ(roclink_server_init(&s, &cfg, g_peers, 0, 1), ROCLINK_E_ARG);
    CHECK_EQ(roclink_server_init(&s, &cfg, NULL, 4, 1), ROCLINK_E_ARG);
    cfg.gateway_id = 0x0001;
    CHECK_EQ(roclink_server_init(&s, &cfg, g_peers, 4, 1), ROCLINK_E_ARG);
}

static void test_uplink_and_ack(void) {
    roclink_server_t s = make_server(true);
    roclink_rx_t rx;
    uint8_t f[ROCLINK_FRAME_MAX_LEN];
    uint8_t ack[16];
    const uint8_t v2[] = {0x24, 0x04, 0x42, 0x01, 0xFF, 0x34, 0x12, 0x07, 0x00};
    size_t n = node_frame(f, ROCLINK_TYPE_DATA, ROCLINK_FLAG_ACK_REQ | ROCLINK_FLAG_LISTEN, NODE,
                          ROCLINK_ADDR_ANY_GATEWAY, 7, 1);
    int an;

    roclink_server_handle(&s, f, n, 100, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_DATA);
    CHECK_EQ(rx.hdr.src, NODE);
    CHECK_EQ(rx.payload_len, 3);
    CHECK(rx.ack_requested);
    CHECK(rx.listening);
    an = roclink_server_build_ack(&s, &rx, ROCLINK_FLAG_MORE, ack, sizeof(ack));
    CHECK_BYTES(ack, an, v2); /* golden vector 2 */
    CHECK_EQ(roclink_server_build_ack(&s, &rx, ROCLINK_FLAG_ACK_REQ, ack, sizeof(ack)),
             ROCLINK_E_ARG);

    roclink_server_handle(&s, f, n, 600, &rx); /* retransmission */
    CHECK_EQ(rx.kind, ROCLINK_RX_DUPLICATE);
    CHECK(rx.ack_requested); /* still ACKed */

    roclink_server_handle(&s, f, n, 1700, &rx); /* same seq, outside the window */
    CHECK_EQ(rx.kind, ROCLINK_RX_DATA);

    n = node_frame(f, ROCLINK_TYPE_DATA, 0, NODE, GW, 8, 1); /* fire-and-forget */
    roclink_server_handle(&s, f, n, 1800, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_DATA);
    CHECK(!rx.ack_requested);
    CHECK(!rx.listening);
    CHECK_EQ(roclink_server_build_ack(&s, &rx, 0, ack, sizeof(ack)), ROCLINK_E_ARG);

    /* node_frame's payload starts with 0x01: a PING, with a body this version
     * ignores (PROTOCOL.md 7.1: later versions may add fields). */
    n = node_frame(f, ROCLINK_TYPE_DATA, ROCLINK_FLAG_LISTEN, NODE, GW, 9, ROCLINK_SCHEMA_CONTROL);
    roclink_server_handle(&s, f, n, 1900, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_CONTROL);
    CHECK(rx.listening);
}

static void test_ping_and_unknown_control(void) {
    roclink_server_t s = make_server(true);
    roclink_rx_t rx;
    uint8_t f[ROCLINK_FRAME_MAX_LEN], ack[16];
    roclink_header_t h;
    const uint8_t ping[] = {ROCLINK_CONTROL_PING};
    const uint8_t unknown[] = {0x02};
    int n;

    h.type = ROCLINK_TYPE_DATA;
    h.flags = ROCLINK_FLAG_ACK_REQ | ROCLINK_FLAG_LISTEN;
    h.net_id = NET;
    h.src = NODE;
    h.dst = ROCLINK_ADDR_ANY_GATEWAY;
    h.seq = 20;
    h.schema = ROCLINK_SCHEMA_CONTROL;
    h.relay = 0;

    /* A link check: ACKed, not delivered. */
    n = roclink_encode(&h, ping, sizeof(ping), f, sizeof(f));
    roclink_server_handle(&s, f, (size_t)n, 100, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_CONTROL);
    CHECK(rx.ack_requested);
    CHECK(roclink_server_build_ack(&s, &rx, 0, ack, sizeof(ack)) == ROCLINK_HEADER_LEN);

    /* An unknown operation, or none: malformed, not ACKed, no state touched. */
    h.seq = 21;
    n = roclink_encode(&h, unknown, sizeof(unknown), f, sizeof(f));
    roclink_server_handle(&s, f, (size_t)n, 200, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_DROP);
    CHECK_EQ(rx.status, ROCLINK_E_MALFORMED);
    CHECK(!rx.ack_requested);
    n = roclink_encode(&h, NULL, 0, f, sizeof(f));
    roclink_server_handle(&s, f, (size_t)n, 300, &rx);
    CHECK_EQ(rx.status, ROCLINK_E_MALFORMED);

    /* seq 21 was never accepted, so a real frame with it isn't a duplicate. */
    n = roclink_encode(&h, ping, sizeof(ping), f, sizeof(f));
    roclink_server_handle(&s, f, (size_t)n, 400, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_CONTROL);
}

static void test_answer_any(void) {
    roclink_server_t s = make_server(false);
    roclink_rx_t rx;
    uint8_t f[ROCLINK_FRAME_MAX_LEN];
    size_t n = node_frame(f, ROCLINK_TYPE_DATA, ROCLINK_FLAG_ACK_REQ, NODE,
                          ROCLINK_ADDR_ANY_GATEWAY, 1, 1);

    roclink_server_handle(&s, f, n, 0, &rx); /* delivered, but another gateway answers */
    CHECK_EQ(rx.kind, ROCLINK_RX_DATA);
    CHECK(!rx.ack_requested);
    CHECK(!rx.listening);

    n = node_frame(f, ROCLINK_TYPE_DATA, ROCLINK_FLAG_ACK_REQ, NODE, GW, 2, 1);
    roclink_server_handle(&s, f, n, 0, &rx); /* addressed to us directly */
    CHECK(rx.ack_requested);
    CHECK(rx.listening);
}

static void test_filtering(void) {
    roclink_server_t s = make_server(true);
    roclink_rx_t rx;
    uint8_t f[ROCLINK_FRAME_MAX_LEN];
    size_t n;

    n = node_frame(f, ROCLINK_TYPE_DATA, 0, NODE, 0xFF02, 1, 1); /* other gateway */
    roclink_server_handle(&s, f, n, 0, &rx);
    CHECK_EQ(rx.status, ROCLINK_E_ADDR);

    n = node_frame(f, ROCLINK_TYPE_DATA, 0, 0xFF02, GW, 1, 1); /* from a gateway */
    roclink_server_handle(&s, f, n, 0, &rx);
    CHECK_EQ(rx.status, ROCLINK_E_ADDR);

    n = node_frame(f, ROCLINK_TYPE_DATA, 0, NODE, GW, 1, 1);
    f[2] = NET + 1;
    roclink_server_handle(&s, f, n, 0, &rx);
    CHECK_EQ(rx.status, ROCLINK_E_NET);

    n = node_frame(f, ROCLINK_TYPE_DATA, 0, NODE, GW, 1, 1);
    f[1] = ROCLINK_FLAG_RELAYED; /* 12-byte frame: header 11 + 1 payload byte */
    roclink_server_handle(&s, f, n, 0, &rx);
    CHECK_EQ(rx.status, ROCLINK_E_RELAYED);

    n = node_frame(f, ROCLINK_TYPE_ACK, 0, NODE, GW, 0x55, 0);
    roclink_server_handle(&s, f, n, 0, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_ACK);
    CHECK_EQ(rx.hdr.seq, 0x55);
}

static void test_downlink_seq_is_per_node(void) {
    roclink_server_t s = make_server(true);
    uint8_t f[ROCLINK_FRAME_MAX_LEN];
    uint8_t seq_a = 0, seq_a2 = 0, seq_b = 0;
    const uint8_t cmd[] = {0x3C, 0x00};
    int i;

    CHECK(roclink_server_build_data(&s, NODE, 10, cmd, 2, ROCLINK_FLAG_ACK_REQ, 0, f, sizeof(f),
                                    &seq_a) > 0);
    CHECK_EQ(f[3] | (f[4] << 8), GW);
    CHECK_EQ(f[5] | (f[6] << 8), NODE);
    /* 300 downlinks to another node must not move NODE's sequence. */
    for (i = 0; i < 300; ++i) {
        CHECK(roclink_server_build_data(&s, 0x0777, 10, cmd, 2, 0, 0, f, sizeof(f), &seq_b) > 0);
    }
    CHECK(roclink_server_build_data(&s, NODE, 10, cmd, 2, 0, 0, f, sizeof(f), &seq_a2) > 0);
    CHECK_EQ(seq_a2, (uint8_t)(seq_a + 1));

    CHECK_EQ(roclink_server_build_data(&s, NODE, 0, cmd, 2, 0, 0, f, sizeof(f), NULL),
             ROCLINK_E_ARG);
    CHECK_EQ(roclink_server_build_data(&s, NODE, 1, cmd, 2, ROCLINK_FLAG_LISTEN, 0, f, sizeof(f),
                                       NULL),
             ROCLINK_E_ARG);
    CHECK_EQ(roclink_server_build_data(&s, ROCLINK_ADDR_BROADCAST, 1, cmd, 2, 0, 0, f, sizeof(f),
                                       NULL),
             ROCLINK_E_ARG);

    /* With a hub, the hub assigns the seq: the gateway just uses it. */
    CHECK_EQ(roclink_server_build_data_seq(&s, NODE, 10, cmd, 2, ROCLINK_FLAG_ACK_REQ, 0x90, f,
                                           sizeof(f)),
             11);
    CHECK_EQ(f[7], 0x90);
    CHECK_EQ(f[1], ROCLINK_FLAG_ACK_REQ);
    CHECK(roclink_server_build_data(&s, NODE, 10, cmd, 2, 0, 0, f, sizeof(f), &seq_b) > 0);
    CHECK_EQ(seq_b, (uint8_t)(seq_a2 + 1)); /* the per-node counter was untouched */
    CHECK_EQ(roclink_server_build_data_seq(&s, NODE, 0, cmd, 2, 0, 1, f, sizeof(f)), ROCLINK_E_ARG);
    CHECK_EQ(roclink_server_build_data_seq(&s, NODE, 1, cmd, 2, ROCLINK_FLAG_LISTEN, 1, f, sizeof(f)),
             ROCLINK_E_ARG);
}

static void test_peer_eviction(void) {
    roclink_server_t s = make_server(true);
    roclink_rx_t rx;
    uint8_t f[ROCLINK_FRAME_MAX_LEN];
    size_t n;
    uint16_t node;

    for (node = 1; node <= 4; ++node) { /* fill the 4-entry table; node 1 is oldest */
        n = node_frame(f, ROCLINK_TYPE_DATA, 0, node, GW, 50, 1);
        roclink_server_handle(&s, f, n, node * 10u, &rx);
    }
    n = node_frame(f, ROCLINK_TYPE_DATA, 0, 5, GW, 50, 1); /* evicts node 1 */
    roclink_server_handle(&s, f, n, 60, &rx);
    n = node_frame(f, ROCLINK_TYPE_DATA, 0, 2, GW, 50, 1); /* node 2 still tracked */
    roclink_server_handle(&s, f, n, 70, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_DUPLICATE);
    n = node_frame(f, ROCLINK_TYPE_DATA, 0, 1, GW, 50, 1); /* node 1 was forgotten */
    roclink_server_handle(&s, f, n, 80, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_DATA);
}

static void test_downlink_never_evicts(void) {
    roclink_server_t s = make_server(true);
    roclink_rx_t rx;
    uint8_t f[ROCLINK_FRAME_MAX_LEN];
    const uint8_t cmd[] = {1};
    uint8_t seq = 0;
    size_t n;
    uint16_t node;

    /* A downlink to an untracked node takes a free slot. */
    CHECK(roclink_server_build_data(&s, 0x0100, 1, cmd, 1, 0, 0, f, sizeof(f), &seq) > 0);
    for (node = 1; node <= 3; ++node) { /* fill the rest of the 4-entry table */
        n = node_frame(f, ROCLINK_TYPE_DATA, 0, node, GW, 50, 1);
        roclink_server_handle(&s, f, n, node * 10u, &rx);
    }
    /* Full: a downlink to another untracked node is refused, not evicting anyone. */
    CHECK_EQ(roclink_server_build_data(&s, 0x0200, 1, cmd, 1, 0, 40, f, sizeof(f), NULL),
             ROCLINK_E_PEERS_FULL);
    for (node = 1; node <= 3; ++node) { /* every node's duplicate state survived */
        n = node_frame(f, ROCLINK_TYPE_DATA, 0, node, GW, 50, 1);
        roclink_server_handle(&s, f, n, 50, &rx);
        CHECK_EQ(rx.kind, ROCLINK_RX_DUPLICATE);
    }
    /* Tracked nodes still get downlink, with their own sequence. */
    CHECK(roclink_server_build_data(&s, 0x0100, 1, cmd, 1, 0, 60, f, sizeof(f), &rx.hdr.seq) > 0);
    CHECK_EQ(rx.hdr.seq, (uint8_t)(seq + 1));
    /* A frame from a node still evicts the least recently seen one, as before. */
    n = node_frame(f, ROCLINK_TYPE_DATA, 0, 0x0200, GW, 50, 1);
    roclink_server_handle(&s, f, n, 70, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_DATA);
}

static void test_clock_wrap(void) {
    roclink_server_t s = make_server(true);
    roclink_rx_t rx;
    uint8_t f[ROCLINK_FRAME_MAX_LEN];
    size_t n = node_frame(f, ROCLINK_TYPE_DATA, 0, NODE, GW, 3, 1);
    roclink_server_handle(&s, f, n, 0xFFFFFF00u, &rx);
    roclink_server_handle(&s, f, n, 0x00000100u, &rx); /* 512 ms later, across the wrap */
    CHECK_EQ(rx.kind, ROCLINK_RX_DUPLICATE);
}

int main(void) {
    RUN(test_init_rejects);
    RUN(test_uplink_and_ack);
    RUN(test_ping_and_unknown_control);
    RUN(test_answer_any);
    RUN(test_filtering);
    RUN(test_downlink_seq_is_per_node);
    RUN(test_peer_eviction);
    RUN(test_downlink_never_evicts);
    RUN(test_clock_wrap);
    return REPORT();
}
