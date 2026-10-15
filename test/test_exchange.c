/* Client and server talking to each other through byte buffers: the full
 * exchanges from docs/PROTOCOL.md section 7, with a lost frame. */
#include "roclink.h"

#include "check.h"

#define NET 0x42
#define NODE 0x1234
#define GW 0xFF01

typedef struct {
    roclink_client_t client;
    roclink_server_t server;
    roclink_peer_t peers[8];
} pair_t;

static void setup(pair_t *p) {
    roclink_client_config_t cc;
    roclink_server_config_t sc;
    cc.net_id = NET;
    cc.node_id = NODE;
    cc.gateway_id = ROCLINK_ADDR_ANY_GATEWAY;
    CHECK_EQ(roclink_client_init(&p->client, &cc, 200), ROCLINK_OK);
    roclink_server_config_default(&sc);
    sc.net_id = NET;
    sc.gateway_id = GW;
    CHECK_EQ(roclink_server_init(&p->server, &sc, p->peers, 8, 99), ROCLINK_OK);
}

static void test_uplink_ack_with_downlink(void) {
    pair_t p;
    roclink_rx_t srx, crx;
    uint8_t up[64], ack[16], dl[64], nack[16];
    const uint8_t reading[] = {0x10, 0x09, 0x88, 0x13}; /* app bytes; roclink doesn't care */
    const uint8_t cmd[] = {0x3C, 0x00};
    uint8_t dl_seq = 0;
    int n, an, dn, nn;
    setup(&p);

    /* node -> gateway: DATA with ACK_REQ */
    n = roclink_client_build_data(&p.client, 1, reading, sizeof(reading), ROCLINK_SEND_ACK, up,
                                  sizeof(up));
    CHECK(n > 0);
    roclink_server_handle(&p.server, up, (size_t)n, 1000, &srx);
    CHECK_EQ(srx.kind, ROCLINK_RX_DATA);
    CHECK_EQ(srx.payload_len, sizeof(reading));
    CHECK(memcmp(srx.payload, reading, sizeof(reading)) == 0);
    CHECK(srx.listening);

    /* gateway -> node: ACK with MORE, then the queued downlink asking for an ACK */
    an = roclink_server_build_ack(&p.server, &srx, ROCLINK_FLAG_MORE, ack, sizeof(ack));
    dn = roclink_server_build_data(&p.server, srx.hdr.src, 10, cmd, sizeof(cmd),
                                   ROCLINK_FLAG_ACK_REQ, 1000, dl, sizeof(dl), &dl_seq);
    CHECK(an > 0 && dn > 0);

    roclink_client_handle(&p.client, ack, (size_t)an, &crx);
    CHECK_EQ(crx.kind, ROCLINK_RX_ACK);
    CHECK(crx.more);
    roclink_client_handle(&p.client, dl, (size_t)dn, &crx);
    CHECK_EQ(crx.kind, ROCLINK_RX_DATA);
    CHECK_EQ(crx.hdr.schema, 10);
    CHECK(crx.ack_requested);

    /* node -> gateway: ACK of the downlink */
    nn = roclink_client_build_ack(&p.client, &crx, nack, sizeof(nack));
    roclink_server_handle(&p.server, nack, (size_t)nn, 1100, &srx);
    CHECK_EQ(srx.kind, ROCLINK_RX_ACK);
    CHECK_EQ(srx.hdr.seq, dl_seq);
}

static void test_lost_ack_retransmission(void) {
    pair_t p;
    roclink_rx_t srx, crx;
    uint8_t up[64], ack[16];
    int n, an;
    setup(&p);

    n = roclink_client_build_data(&p.client, 2, NULL, 0, ROCLINK_SEND_ACK, up, sizeof(up));
    roclink_server_handle(&p.server, up, (size_t)n, 0, &srx);
    CHECK_EQ(srx.kind, ROCLINK_RX_DATA);
    an = roclink_server_build_ack(&p.server, &srx, 0, ack, sizeof(ack));
    CHECK(an > 0);
    /* ...the ACK is lost on the air; the node resends the identical bytes. */
    CHECK(roclink_client_awaiting_ack(&p.client));
    roclink_server_handle(&p.server, up, (size_t)n, 400, &srx);
    CHECK_EQ(srx.kind, ROCLINK_RX_DUPLICATE); /* not delivered twice */
    an = roclink_server_build_ack(&p.server, &srx, 0, ack, sizeof(ack));
    roclink_client_handle(&p.client, ack, (size_t)an, &crx);
    CHECK_EQ(crx.kind, ROCLINK_RX_ACK);
    CHECK(!roclink_client_awaiting_ack(&p.client));
}

static void test_reoffered_downlink(void) {
    pair_t p;
    roclink_rx_t crx;
    uint8_t dl[64];
    const uint8_t cmd[] = {1};
    int dn, i;
    setup(&p);

    dn = roclink_server_build_data(&p.server, NODE, 5, cmd, 1, ROCLINK_FLAG_ACK_REQ, 0, dl,
                                   sizeof(dl), NULL);
    roclink_client_handle(&p.client, dl, (size_t)dn, &crx);
    CHECK_EQ(crx.kind, ROCLINK_RX_DATA);
    for (i = 0; i < 3; ++i) { /* node's ACKs keep getting lost: same bytes re-offered */
        roclink_client_handle(&p.client, dl, (size_t)dn, &crx);
        CHECK_EQ(crx.kind, ROCLINK_RX_DUPLICATE);
        CHECK(crx.ack_requested);
    }
    dn = roclink_server_build_data(&p.server, NODE, 5, cmd, 1, 0, 0, dl, sizeof(dl), NULL);
    roclink_client_handle(&p.client, dl, (size_t)dn, &crx); /* the next command is new */
    CHECK_EQ(crx.kind, ROCLINK_RX_DATA);
}

int main(void) {
    RUN(test_uplink_ack_with_downlink);
    RUN(test_lost_ack_retransmission);
    RUN(test_reoffered_downlink);
    return REPORT();
}
