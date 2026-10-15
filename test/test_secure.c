/* Security tests (PROTOCOL.md section 10). Links roclink, roclink_secure and
 * one AES provider (roclink_aes.c).
 * The Makefile builds this once per provider; all must reproduce the same bytes.
 *
 * Expected frames come from tools/gen_vectors.py, which implements the spec
 * independently with OpenSSL (via Python's cryptography package). */
#include "roclink.h"
#include "roclink_secure.h"

#include "check.h"
#include "vectors_secure.h"

#define NET 0x42
#define NODE 0x1234
#define GW 0xFF01

/* Plain frames that the vectors are sealed from. */
static const uint8_t BME[] = {0x01, 0x29, 0x09, 0x18, 0x10, 0xCD, 0x8B,
                              0x01, 0x00, 0xC0, 0x51, 0x02, 0x00};

/* ------------------------------------------------------------ helpers */

static roclink_sec_t node_sec(roclink_sec_level_t level, uint8_t tag_len) {
    roclink_sec_t s;
    roclink_sec_config_t cfg;
    roclink_sec_config_default(&cfg);
    cfg.net_id = NET;
    cfg.addr = NODE;
    cfg.level = level;
    cfg.tag_len = tag_len;
    CHECK_EQ(roclink_sec_init_node(&s, &cfg, NODE_KEY, 0), ROCLINK_OK);
    return s;
}

static roclink_sec_t gw_sec(void) {
    roclink_sec_t s;
    roclink_sec_config_t cfg;
    roclink_sec_config_default(&cfg);
    cfg.net_id = NET;
    cfg.addr = GW;
    CHECK_EQ(roclink_sec_init_gateway(&s, &cfg, MASTER_KEY, 0), ROCLINK_OK);
    return s;
}

/* A counter whose next value is `next`, with its reservation persisted. */
static roclink_fcnt_t counter_at(uint32_t next) {
    roclink_fcnt_t c;
    roclink_fcnt_init(&c, next, 0);
    roclink_fcnt_persisted(&c, roclink_fcnt_to_persist(&c));
    return c;
}

static size_t hdr_frame(uint8_t *out, uint8_t type, uint8_t flags, uint16_t src, uint16_t dst,
                        uint8_t seq, uint8_t schema, const uint8_t *data, size_t len) {
    roclink_header_t h;
    int n;
    memset(&h, 0, sizeof(h));
    h.type = type;
    h.flags = flags;
    h.net_id = NET;
    h.src = src;
    h.dst = dst;
    h.seq = seq;
    h.schema = schema;
    n = roclink_encode(&h, data, len, out, ROCLINK_FRAME_MAX_LEN);
    CHECK(n > 0);
    return n > 0 ? (size_t)n : 0u;
}

/* -------------------------------------------------------- primitives */

static void test_primitives(void) {
    /* RFC 4493 section 4, examples 1 and 2 */
    static const uint8_t k[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                  0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
    static const uint8_t m2[16] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
                                   0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};
    static const uint8_t t1[16] = {0xbb, 0x1d, 0x69, 0x29, 0xe9, 0x59, 0x37, 0x28,
                                   0x7f, 0xa3, 0x7d, 0x12, 0x9b, 0x75, 0x67, 0x46};
    static const uint8_t t2[16] = {0x07, 0x0a, 0x16, 0xb4, 0x6b, 0x4d, 0x41, 0x44,
                                   0xf7, 0x9b, 0xdd, 0x9d, 0xd0, 0x4a, 0x28, 0x7c};
    /* RFC 4493 examples 3 and 4: 40 bytes (a short last block) and 64 (four full ones) */
    static const uint8_t m4[64] = {
        0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
        0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
        0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
        0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10};
    static const uint8_t t3[16] = {0xdf, 0xa6, 0x67, 0x47, 0xde, 0x9a, 0xe6, 0x30,
                                   0x30, 0xca, 0x32, 0x61, 0x14, 0x97, 0xc8, 0x27};
    static const uint8_t t4[16] = {0x51, 0xf0, 0xbe, 0xbf, 0x7e, 0x3b, 0x9d, 0x92,
                                   0xfc, 0x49, 0x74, 0x17, 0x79, 0x36, 0x3c, 0xfe};
    /* RFC 3610 packet vector #1: 8 bytes AAD, 23 bytes payload, 8-byte tag */
    static const uint8_t ccm_out[] = {0x58, 0x8C, 0x97, 0x9A, 0x61, 0xC6, 0x63, 0xD2, 0xF0, 0x66,
                                      0xD0, 0xC2, 0xC0, 0xF9, 0x89, 0x80, 0x6D, 0x5F, 0x6B, 0x61,
                                      0xDA, 0xC3, 0x84, 0x17, 0xE8, 0xD1, 0x2C, 0xFD, 0xF9, 0x26,
                                      0xE0};
    static const uint8_t ccm_nonce[13] = {0x00, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00,
                                          0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
    uint8_t ccm_key[16], aad[8], buf[40], buf2[40], mac[16];
    uint8_t i;
    for (i = 0; i < 16; ++i) ccm_key[i] = (uint8_t)(0xC0 + i);
    for (i = 0; i < 8; ++i) aad[i] = i;
    for (i = 0; i < 23; ++i) buf[i] = (uint8_t)(8 + i);

    /* The provider itself: the FIPS-197 known answer. */
    CHECK_EQ(roclink_crypto_selftest(), 0);

    CHECK_EQ(roclink_crypto_cmac(k, NULL, 0, mac), 0);
    CHECK_BYTES(mac, 16, t1);
    CHECK_EQ(roclink_crypto_cmac(k, m2, sizeof(m2), mac), 0);
    CHECK_BYTES(mac, 16, t2);
    CHECK_EQ(roclink_crypto_cmac(k, m4, 40, mac), 0);
    CHECK_BYTES(mac, 16, t3);
    CHECK_EQ(roclink_crypto_cmac(k, m4, 64, mac), 0);
    CHECK_BYTES(mac, 16, t4);

    CHECK_EQ(roclink_crypto_ccm_seal(ccm_key, ccm_nonce, aad, 8, NULL, 0, buf, 23, 8), 0);
    CHECK_BYTES(buf, 31, ccm_out);
    CHECK_EQ(roclink_crypto_ccm_open(ccm_key, ccm_nonce, aad, 8, NULL, 0, buf, 23, 8), 0);
    CHECK_EQ(buf[0], 8);
    CHECK_EQ(buf[22], 30);
    /* The AAD in two pieces, split anywhere, is the same AAD. */
    for (i = 0; i < 23; ++i) buf2[i] = (uint8_t)(8 + i);
    CHECK_EQ(roclink_crypto_ccm_seal(ccm_key, ccm_nonce, aad, 3, aad + 3, 5, buf2, 23, 8), 0);
    CHECK_BYTES(buf2, 31, ccm_out);
    CHECK_EQ(roclink_crypto_ccm_open(ccm_key, ccm_nonce, NULL, 0, aad, 8, buf2, 23, 8), 0);
    memcpy(buf, ccm_out, sizeof(ccm_out));
    buf[30] ^= 1; /* tag bit flip */
    CHECK_EQ(roclink_crypto_ccm_open(ccm_key, ccm_nonce, aad, 8, NULL, 0, buf, 23, 8),
             ROCLINK_CCM_BAD_TAG);
    for (i = 0; i < 23; ++i) CHECK_EQ(buf[i], 0); /* unauthenticated plaintext is wiped */
    memcpy(buf, ccm_out, sizeof(ccm_out));
    aad[0] ^= 1; /* AAD changed */
    CHECK_EQ(roclink_crypto_ccm_open(ccm_key, ccm_nonce, aad, 8, NULL, 0, buf, 23, 8),
             ROCLINK_CCM_BAD_TAG);
    /* Arguments CCM can't encode. */
    CHECK(roclink_crypto_ccm_seal(ccm_key, ccm_nonce, aad, 8, NULL, 0, buf, 23, 5) < 0);
    CHECK(roclink_crypto_ccm_seal(ccm_key, ccm_nonce, aad, 8, NULL, 0, buf, 23, 2) < 0);
    CHECK(roclink_crypto_ccm_seal(ccm_key, ccm_nonce, NULL, 8, NULL, 0, buf, 23, 8) < 0);
}

static void test_key_derivation(void) {
    uint8_t k[16];
    CHECK_EQ(roclink_sec_derive_node_key(MASTER_KEY, NET, NODE, 0, k), ROCLINK_OK);
    CHECK_BYTES(k, 16, NODE_KEY);
    CHECK_EQ(roclink_sec_derive_node_key(MASTER_KEY, NET, NODE, 1, k), ROCLINK_OK);
    CHECK(memcmp(k, NODE_KEY, 16) != 0); /* each generation is a different key */
    CHECK_EQ(roclink_sec_derive_node_key(MASTER_KEY, NET, GW, 0, k), ROCLINK_E_ARG);
    CHECK_EQ(roclink_sec_derive_node_key(MASTER_KEY, NET, NODE, 16, k), ROCLINK_E_ARG);

    /* The ratchet: CK(0) -> CK(1), independently generated. */
    CHECK_EQ(roclink_sec_chain_advance(NODE_KEY, NET, NODE, 0, 0, 1, k), ROCLINK_OK);
    CHECK_BYTES(k, 16, CHAIN_KEY_1);
    CHECK_EQ(roclink_sec_chain_advance(NODE_KEY, NET, NODE, 0, 0, 0, k), ROCLINK_OK);
    CHECK_BYTES(k, 16, NODE_KEY); /* zero steps */
    CHECK_EQ(roclink_sec_chain_advance(NODE_KEY, NET, NODE, 0, 3, 2, k), ROCLINK_E_ARG);
    CHECK_EQ(roclink_sec_chain_advance(NODE_KEY, NET, NODE, 0, 0, ROCLINK_MAX_EPOCH_SKIP + 1, k),
             ROCLINK_E_KEY);
}

/* ---------------------------------------------------- frame counter */

static void test_frame_counter(void) {
    roclink_fcnt_t c;
    uint32_t v = 0;
    uint32_t i;

    roclink_fcnt_init(&c, 0, 4);
    CHECK_EQ(roclink_fcnt_take(&c, &v), ROCLINK_E_PERSIST); /* first use reserves a block */
    CHECK_EQ(roclink_fcnt_to_persist(&c), 4);
    roclink_fcnt_persisted(&c, 4);
    for (i = 0; i < 4; ++i) {
        CHECK_EQ(roclink_fcnt_take(&c, &v), ROCLINK_OK);
        CHECK_EQ(v, i);
    }
    CHECK_EQ(roclink_fcnt_take(&c, &v), ROCLINK_E_PERSIST);
    CHECK_EQ(roclink_fcnt_to_persist(&c), 8);

    /* After a reset the node resumes from the stored value: 4..7 may have been
     * used, so it continues at 8 and never repeats. */
    roclink_fcnt_init(&c, 8, 4);
    roclink_fcnt_persisted(&c, roclink_fcnt_to_persist(&c));
    CHECK_EQ(roclink_fcnt_take(&c, &v), ROCLINK_OK);
    CHECK_EQ(v, 8);

    /* A gateway restarting skips ahead: a restored backup can't re-use values. */
    roclink_fcnt_init(&c, 512, 0);
    CHECK_EQ(roclink_fcnt_skip(&c, ROCLINK_FCNT_RESTART_SKIP), ROCLINK_OK);
    CHECK_EQ(roclink_fcnt_take(&c, &v), ROCLINK_E_PERSIST); /* the new position is stored first */
    CHECK_EQ(roclink_fcnt_to_persist(&c), 512L + 65536L + 256L);
    roclink_fcnt_persisted(&c, roclink_fcnt_to_persist(&c));
    CHECK_EQ(roclink_fcnt_take(&c, &v), ROCLINK_OK);
    CHECK_EQ(v, 512L + 65536L);
    roclink_fcnt_init(&c, 0xFFFF0000u, 0);
    CHECK_EQ(roclink_fcnt_skip(&c, ROCLINK_FCNT_RESTART_SKIP), ROCLINK_E_EXHAUSTED);
    CHECK_EQ(roclink_fcnt_take(&c, &v), ROCLINK_E_EXHAUSTED);
    CHECK_EQ(roclink_fcnt_skip(NULL, 1), ROCLINK_E_ARG);

    roclink_fcnt_init(&c, 0xFFFFFFFEu, 0);
    CHECK_EQ(roclink_fcnt_to_persist(&c), 0xFFFFFFFFL); /* saturates */
    roclink_fcnt_persisted(&c, 0xFFFFFFFFu);
    CHECK_EQ(roclink_fcnt_take(&c, &v), ROCLINK_OK);
    CHECK_EQ(v, 0xFFFFFFFEL);
    CHECK_EQ(roclink_fcnt_take(&c, &v), ROCLINK_E_EXHAUSTED);
}

/* ------------------------------------------------- golden secured frames */

static void test_seal_matches_vectors(void) {
    roclink_sec_t node = node_sec(ROCLINK_SEC_ENC, 8);
    roclink_sec_t node_auth = node_sec(ROCLINK_SEC_AUTH, 4);
    roclink_sec_t gw = gw_sec();
    roclink_fcnt_t nctr, gctr;
    roclink_sec_info_t opener;
    roclink_sec_peer_t gpeer; /* empty: the gateway derives the node's key from the master */
    uint8_t f[ROCLINK_FRAME_MAX_LEN], plain[ROCLINK_FRAME_MAX_LEN];
    const uint8_t cmd[] = {0x3C, 0x00};
    roclink_client_t client;
    roclink_client_config_t cc;
    size_t n;
    int sn;

    /* S1: the BME envelope from a client, enc + 8-byte tag. The counter starts
     * at 1 and asks for its reservation to be persisted first. */
    cc.net_id = NET;
    cc.node_id = NODE;
    cc.gateway_id = ROCLINK_ADDR_ANY_GATEWAY;
    CHECK_EQ(roclink_client_init(&client, &cc, 7), ROCLINK_OK);
    sn = roclink_client_build_data(&client, 0x20, BME, sizeof(BME), ROCLINK_SEND_ACK, f, sizeof(f));
    CHECK_EQ(sn, 22);
    roclink_fcnt_init(&nctr, 1, 0);
    memcpy(plain, f, 22);
    CHECK_EQ(roclink_sec_seal(&node, &nctr, NULL, NULL, f, 22, f, sizeof(f)), ROCLINK_E_PERSIST);
    CHECK(memcmp(plain, f, 22) == 0); /* untouched, so the call can simply be repeated */
    roclink_fcnt_persisted(&nctr, roclink_fcnt_to_persist(&nctr));
    sn = roclink_sec_seal(&node, &nctr, NULL, NULL, f, 22, f, sizeof(f));
    CHECK_BYTES(f, sn, VS1);

    /* S2: auth-only, 4-byte tag, readable payload. */
    nctr = counter_at(2);
    n = hdr_frame(plain, ROCLINK_TYPE_DATA, 0, NODE, GW, 8, 1, (const uint8_t *)"hi", 2);
    sn = roclink_sec_seal(&node_auth, &nctr, NULL, NULL, plain, n, f, sizeof(f));
    CHECK_BYTES(f, sn, VS2);
    CHECK(memcmp(f + 14, "hi", 2) == 0);

    /* S3/S4: gateway ACK + downlink, both bound to the node's S1 (fcnt 1). */
    memset(&opener, 0, sizeof(opener));
    memset(&gpeer, 0, sizeof(gpeer));
    opener.level = ROCLINK_SEC_ENC;
    opener.fcnt = 1;
    gctr = counter_at(0x100);
    n = hdr_frame(plain, ROCLINK_TYPE_ACK, ROCLINK_FLAG_MORE, GW, NODE, 7, 0, NULL, 0);
    sn = roclink_sec_seal(&gw, &gctr, &opener, &gpeer, plain, n, f, sizeof(f));
    CHECK_BYTES(f, sn, VS3);
    n = hdr_frame(plain, ROCLINK_TYPE_DATA, ROCLINK_FLAG_ACK_REQ | ROCLINK_FLAG_DL_QUEUED, GW, NODE,
                  0x90, 10, cmd, sizeof(cmd));
    sn = roclink_sec_seal(&gw, &gctr, &opener, &gpeer, plain, n, f, sizeof(f));
    CHECK_BYTES(f, sn, VS4);
}

/* ------------------------------------------------------ the full exchange */

static void test_secured_exchange(void) {
    roclink_sec_t node = node_sec(ROCLINK_SEC_ENC, 8);
    roclink_sec_t gw = gw_sec();
    roclink_sec_peer_t peer;
    roclink_sec_info_t info, opener;
    roclink_fcnt_t nctr = counter_at(1);
    roclink_server_t server;
    roclink_server_config_t sc;
    roclink_peer_t peers[4];
    roclink_client_t client;
    roclink_client_config_t cc;
    roclink_rx_t rx;
    uint8_t up[ROCLINK_FRAME_MAX_LEN], f[ROCLINK_FRAME_MAX_LEN];
    int un, n;

    memset(&peer, 0, sizeof(peer));
    roclink_server_config_default(&sc);
    sc.net_id = NET;
    sc.gateway_id = GW;
    CHECK_EQ(roclink_server_init(&server, &sc, peers, 4, 5), ROCLINK_OK);
    cc.net_id = NET;
    cc.node_id = NODE;
    cc.gateway_id = ROCLINK_ADDR_ANY_GATEWAY;
    CHECK_EQ(roclink_client_init(&client, &cc, 7), ROCLINK_OK);

    /* node: build + seal S1 */
    un = roclink_client_build_data(&client, 0x20, BME, sizeof(BME), ROCLINK_SEND_ACK, up, sizeof(up));
    un = roclink_sec_seal(&node, &nctr, NULL, NULL, up, (size_t)un, up, sizeof(up));
    CHECK_BYTES(up, un, VS1);

    /* gateway: open, then the plain server logic */
    memcpy(f, up, (size_t)un);
    n = roclink_sec_open(&gw, &peer, f, (size_t)un, &opener);
    CHECK_EQ(n, 22);
    CHECK_EQ(opener.level, ROCLINK_SEC_ENC);
    CHECK_EQ(opener.fcnt, 1);
    CHECK(!opener.retransmission);
    CHECK_EQ(peer.last_fcnt, 1); /* the app persists this now */
    roclink_server_handle_opened(&server, f, (size_t)n, 1000, &opener, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_DATA);
    CHECK_EQ(rx.hdr.schema, 0x20);
    CHECK_EQ(rx.payload_len, sizeof(BME));
    CHECK(memcmp(rx.payload, BME, sizeof(BME)) == 0);
    CHECK_EQ(rx.sec.level, ROCLINK_SEC_ENC);
    CHECK(rx.ack_requested);

    /* The ACK is lost; the node retransmits the identical sealed bytes. */
    memcpy(f, up, (size_t)un);
    n = roclink_sec_open(&gw, &peer, f, (size_t)un, &info);
    CHECK_EQ(n, 22);
    CHECK(info.retransmission);
    roclink_server_handle_opened(&server, f, (size_t)n, 1500, &info, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_DUPLICATE); /* the frame counter decides */
    CHECK(rx.ack_requested);

    /* node: receives the gateway's S3 (ACK, MORE) and S4 (downlink) */
    memcpy(f, VS3, sizeof(VS3));
    n = roclink_sec_open(&node, NULL, f, sizeof(VS3), &info);
    CHECK_EQ(n, 9);
    roclink_client_handle(&client, f, (size_t)n, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_ACK);
    CHECK(rx.more);
    memcpy(f, VS4, sizeof(VS4));
    n = roclink_sec_open(&node, NULL, f, sizeof(VS4), &info);
    CHECK_EQ(n, 11);
    roclink_client_handle(&client, f, (size_t)n, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_DATA);
    CHECK_EQ(rx.hdr.schema, 10);
    CHECK_EQ(rx.payload[0], 0x3C);

    /* Replaying S4 inside the same reply is refused. */
    memcpy(f, VS4, sizeof(VS4));
    CHECK_EQ(roclink_sec_open(&node, NULL, f, sizeof(VS4), &info), ROCLINK_E_REPLAY);

    /* The node ACKs the downlink (secured, its next counter); the gateway opens it. */
    n = roclink_client_build_ack(&client, &rx, f, sizeof(f));
    n = roclink_sec_seal(&node, &nctr, NULL, NULL, f, (size_t)n, f, sizeof(f));
    CHECK(n > 0);
    n = roclink_sec_open(&gw, &peer, f, (size_t)n, &info);
    CHECK_EQ(n, 9);
    CHECK_EQ(info.fcnt, 2);
    roclink_server_handle_opened(&server, f, (size_t)n, 1600, &info, &rx);
    CHECK_EQ(rx.kind, ROCLINK_RX_ACK);
    CHECK_EQ(rx.hdr.seq, 0x90);
}

/* A new secured frame that happens to reuse the last seq is not a duplicate. */
static void test_secured_dedupe_ignores_seq(void) {
    roclink_sec_t node = node_sec(ROCLINK_SEC_ENC, 8);
    roclink_sec_t gw = gw_sec();
    roclink_sec_peer_t peer;
    roclink_sec_info_t info;
    roclink_fcnt_t nctr = counter_at(10);
    roclink_server_t server;
    roclink_server_config_t sc;
    roclink_peer_t peers[2];
    roclink_rx_t rx;
    uint8_t f[ROCLINK_FRAME_MAX_LEN];
    int i, n;
    memset(&peer, 0, sizeof(peer));
    roclink_server_config_default(&sc);
    sc.net_id = NET;
    sc.gateway_id = GW;
    CHECK_EQ(roclink_server_init(&server, &sc, peers, 2, 9), ROCLINK_OK);
    for (i = 0; i < 2; ++i) {
        n = (int)hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, GW, 33, 1, NULL, 0); /* same seq */
        n = roclink_sec_seal(&node, &nctr, NULL, NULL, f, (size_t)n, f, sizeof(f));
        n = roclink_sec_open(&gw, &peer, f, (size_t)n, &info);
        roclink_server_handle_opened(&server, f, (size_t)n, 100, &info, &rx);
        CHECK_EQ(rx.kind, ROCLINK_RX_DATA);
    }
}

/* --------------------------------------------------------------- rejection */

static void test_tampering_is_rejected(void) {
    roclink_sec_t gw = gw_sec();
    roclink_sec_peer_t peer;
    roclink_sec_info_t info;
    uint8_t f[sizeof(VS1)];
    size_t i;
    /* Flip each byte in turn: header fields, security header, ciphertext, tag. */
    for (i = 0; i < sizeof(VS1); ++i) {
        int rc;
        memset(&peer, 0, sizeof(peer));
        memcpy(f, VS1, sizeof(VS1));
        f[i] ^= 0x01;
        rc = roclink_sec_open(&gw, &peer, f, sizeof(f), &info);
        CHECK(rc < 0);
        CHECK_EQ(peer.state, 0); /* a rejected frame changes no state */
    }
    memset(&peer, 0, sizeof(peer));
    memcpy(f, VS1, sizeof(VS1));
    f[20] ^= 0x80; /* ciphertext */
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(f), &info), ROCLINK_E_AUTH);
    memcpy(f, VS1, sizeof(VS1));
    f[8] = 0x21; /* schema */
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(f), &info), ROCLINK_E_AUTH);
    memcpy(f, VS1, sizeof(VS1));
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(f) - 1, &info), ROCLINK_E_AUTH); /* truncated */
}

static void test_replay_and_keys(void) {
    roclink_sec_t gw = gw_sec();
    roclink_sec_peer_t peer;
    roclink_sec_info_t info;
    uint8_t f[sizeof(VS1)];
    memset(&peer, 0, sizeof(peer));
    peer.state = 1; /* as persisted from before: last accepted fcnt 5 */
    peer.last_fcnt = 5;
    memcpy(f, VS1, sizeof(VS1)); /* fcnt 1 */
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(f), &info), ROCLINK_E_REPLAY);

    memset(&peer, 0, sizeof(peer));
    memcpy(f, VS1, sizeof(VS1));
    f[9] = (uint8_t)((f[9] & ~ROCLINK_SECCTRL_KEY_GEN_MASK) | 3); /* unknown generation */
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(f), &info), ROCLINK_E_KEY);
    memcpy(f, VS1, sizeof(VS1));
    f[9] |= 0x40; /* suite 1: not supported */
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(f), &info), ROCLINK_E_KEY);
    memcpy(f, VS1, sizeof(VS1));
    f[2] = NET + 1;
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(f), &info), ROCLINK_E_NET);
    memcpy(f, VS1, sizeof(VS1));
    f[5] = 0x02; /* dst 0xFF02 when this is gateway 0xFF01 */
    f[6] = 0xFF;
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(f), &info), ROCLINK_E_ADDR);
    memcpy(f, VS1, sizeof(VS1));
    CHECK_EQ(roclink_sec_open(&gw, NULL, f, sizeof(f), &info), ROCLINK_E_ARG); /* gateway needs peer state */
}

static void test_key_rotation(void) {
    roclink_sec_t node0 = node_sec(ROCLINK_SEC_ENC, 8);
    roclink_sec_t node1;
    roclink_sec_t gw;
    roclink_sec_config_t cfg;
    roclink_sec_peer_t peer;
    roclink_sec_info_t info;
    roclink_fcnt_t c0 = counter_at(50), c1 = counter_at(0);
    uint8_t master1[16], key1[16], f[64];
    int n;
    memset(master1, 0xA5, sizeof(master1));
    CHECK_EQ(roclink_sec_derive_node_key(master1, NET, NODE, 1, key1), ROCLINK_OK);
    roclink_sec_config_default(&cfg);
    cfg.net_id = NET;
    cfg.addr = NODE;
    CHECK_EQ(roclink_sec_init_node(&node1, &cfg, key1, 1), ROCLINK_OK);
    cfg.addr = GW;
    CHECK_EQ(roclink_sec_init_gateway(&gw, &cfg, master1, 1), ROCLINK_OK); /* gen 1 current */
    CHECK_EQ(roclink_sec_add_key(&gw, MASTER_KEY, 2), ROCLINK_E_ARG);       /* only the previous one */
    CHECK_EQ(roclink_sec_add_key(&gw, MASTER_KEY, 1), ROCLINK_E_ARG);
    CHECK_EQ(roclink_sec_add_key(&gw, MASTER_KEY, 0), ROCLINK_OK);          /* gen 0 still accepted */
    CHECK_EQ(roclink_sec_add_key(&gw, MASTER_KEY, 0), ROCLINK_E_ARG);       /* at most two */
    memset(&peer, 0, sizeof(peer));

    n = (int)hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, GW, 1, 1, NULL, 0);
    n = roclink_sec_seal(&node0, &c0, NULL, NULL, f, (size_t)n, f, sizeof(f));
    CHECK(roclink_sec_open(&gw, &peer, f, (size_t)n, &info) > 0); /* not yet migrated: gen 0 ok */
    CHECK_EQ(info.key_gen, 0);

    n = (int)hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, GW, 2, 1, NULL, 0);
    n = roclink_sec_seal(&node1, &c1, NULL, NULL, f, (size_t)n, f, sizeof(f));
    CHECK(roclink_sec_open(&gw, &peer, f, (size_t)n, &info) > 0); /* migrated: counter restarts */
    CHECK_EQ(info.key_gen, 1);
    CHECK_EQ(info.fcnt, 0);

    n = (int)hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, GW, 3, 1, NULL, 0);
    n = roclink_sec_seal(&node0, &c0, NULL, NULL, f, (size_t)n, f, sizeof(f));
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, (size_t)n, &info), ROCLINK_E_KEY); /* no going back */
}

static void test_policy_and_downgrade(void) {
    roclink_sec_t gw = gw_sec();
    roclink_sec_t node = node_sec(ROCLINK_SEC_ENC, 8);
    roclink_sec_peer_t peer;
    roclink_sec_info_t info;
    roclink_fcnt_t nctr = counter_at(1);
    uint8_t f[ROCLINK_FRAME_MAX_LEN];
    size_t n;
    memset(&peer, 0, sizeof(peer));

    /* Plaintext to a require-enc gateway: refused. */
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, GW, 1, 1, NULL, 0);
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, n, &info), ROCLINK_E_POLICY);
    /* auth-only (S2) to require-enc: refused; per-node override to require-auth: accepted. */
    memcpy(f, VS2, sizeof(VS2));
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(VS2), &info), ROCLINK_E_POLICY);
    peer.policy = ROCLINK_POLICY_REQUIRE_AUTH;
    memcpy(f, VS2, sizeof(VS2));
    /* S2 has a 4-byte tag: refused until the node is allowed short tags too. */
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(VS2), &info), ROCLINK_E_POLICY);
    CHECK_EQ(peer.state, 0);
    peer.min_tag = 4;
    memcpy(f, VS2, sizeof(VS2));
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(VS2), &info), 11);
    CHECK_EQ(info.level, ROCLINK_SEC_AUTH);
    CHECK(memcmp(f + 9, "hi", 2) == 0);
    /* Explicit per-node opt-out: plaintext passes through untouched. */
    peer.policy = ROCLINK_POLICY_ALLOW_NONE;
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, GW, 2, 1, NULL, 0);
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, n, &info), (int)n);
    CHECK_EQ(info.level, ROCLINK_SEC_NONE);

    /* Node: a forged plaintext ACK can't stand in for a secured one. */
    n = hdr_frame(f, ROCLINK_TYPE_DATA, ROCLINK_FLAG_ACK_REQ, NODE, GW, 3, 1, NULL, 0);
    CHECK(roclink_sec_seal(&node, &nctr, NULL, NULL, f, n, f, sizeof(f)) > 0);
    n = hdr_frame(f, ROCLINK_TYPE_ACK, 0, GW, NODE, 3, 0, NULL, 0);
    CHECK_EQ(roclink_sec_open(&node, NULL, f, n, &info), ROCLINK_E_POLICY);
}

static void test_downlink_replay_after_new_uplink(void) {
    roclink_sec_t node = node_sec(ROCLINK_SEC_ENC, 8);
    roclink_sec_info_t info;
    roclink_fcnt_t nctr = counter_at(1);
    uint8_t f[ROCLINK_FRAME_MAX_LEN];
    size_t n;

    /* Not listening (nothing sent yet): any gateway frame is unexpected. */
    memcpy(f, VS4, sizeof(VS4));
    CHECK_EQ(roclink_sec_open(&node, NULL, f, sizeof(VS4), &info), ROCLINK_E_UNEXPECTED);

    /* Opener with fcnt 1: the recorded S4 (bound to 1) is accepted... */
    n = hdr_frame(f, ROCLINK_TYPE_DATA, ROCLINK_FLAG_LISTEN, NODE, 0, 1, 1, NULL, 0);
    CHECK(roclink_sec_seal(&node, &nctr, NULL, NULL, f, n, f, sizeof(f)) > 0);
    memcpy(f, VS4, sizeof(VS4));
    CHECK_EQ(roclink_sec_open(&node, NULL, f, sizeof(VS4), &info), 11);

    /* ...but after the next uplink (fcnt 2) the same recording fails: this is
     * the "open valve" replay, rejected with no stored state on the node. */
    n = hdr_frame(f, ROCLINK_TYPE_DATA, ROCLINK_FLAG_LISTEN, NODE, 0, 2, 1, NULL, 0);
    CHECK(roclink_sec_seal(&node, &nctr, NULL, NULL, f, n, f, sizeof(f)) > 0);
    memcpy(f, VS4, sizeof(VS4));
    CHECK_EQ(roclink_sec_open(&node, NULL, f, sizeof(VS4), &info), ROCLINK_E_AUTH);

    /* A fire-and-forget uplink closes the reply opportunity altogether. */
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, 0, 3, 1, NULL, 0);
    CHECK(roclink_sec_seal(&node, &nctr, NULL, NULL, f, n, f, sizeof(f)) > 0);
    memcpy(f, VS4, sizeof(VS4));
    CHECK_EQ(roclink_sec_open(&node, NULL, f, sizeof(VS4), &info), ROCLINK_E_UNEXPECTED);
}

static void test_close_window(void) {
    roclink_sec_t node = node_sec(ROCLINK_SEC_ENC, 8);
    roclink_sec_t gw = gw_sec();
    roclink_sec_info_t info;
    roclink_fcnt_t nctr = counter_at(1);
    uint8_t f[ROCLINK_FRAME_MAX_LEN];
    size_t n;

    /* Opener with fcnt 1, as before: S4 (bound to 1) would be accepted... */
    n = hdr_frame(f, ROCLINK_TYPE_DATA, ROCLINK_FLAG_LISTEN, NODE, 0, 1, 1, NULL, 0);
    CHECK(roclink_sec_seal(&node, &nctr, NULL, NULL, f, n, f, sizeof(f)) > 0);
    /* ...but the node stopped listening: a reply held back and delivered late is refused. */
    roclink_sec_close_window(&node);
    memcpy(f, VS4, sizeof(VS4));
    CHECK_EQ(roclink_sec_open(&node, NULL, f, sizeof(VS4), &info), ROCLINK_E_UNEXPECTED);

    roclink_sec_close_window(NULL);  /* harmless */
    roclink_sec_close_window(&gw);   /* no effect on a gateway */
}

/* ------------------------------------------------------- the ratchet */

/* True if `key` appears anywhere in the bytes of `obj`. */
static bool contains_key(const void *obj, size_t n, const uint8_t key[16]) {
    const uint8_t *b = (const uint8_t *)obj;
    size_t i;
    for (i = 0; i + 16 <= n; ++i) {
        if (memcmp(b + i, key, 16) == 0) return true;
    }
    return false;
}

static void test_ratchet_epoch_change(void) {
    roclink_sec_t node = node_sec(ROCLINK_SEC_ENC, 8);
    roclink_sec_t gw = gw_sec();
    roclink_sec_peer_t peer;
    roclink_sec_info_t info;
    roclink_fcnt_t c = counter_at(0xFFFF); /* the last counter of epoch 0 */
    uint8_t f[ROCLINK_FRAME_MAX_LEN], ck[16];
    uint32_t e = 0;
    size_t n;
    int sn;
    memset(&peer, 0, sizeof(peer));

    /* Last frame of epoch 0. */
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, 0, 8, 1, NULL, 0);
    sn = roclink_sec_seal(&node, &c, NULL, NULL, f, n, f, sizeof(f));
    CHECK(sn > 0);
    CHECK(roclink_sec_open(&gw, &peer, f, (size_t)sn, &info) > 0);
    CHECK_EQ(peer.epoch, 0);

    /* The next counter is in epoch 1: the node must persist CK(1) first. */
    n = hdr_frame(f, ROCLINK_TYPE_DATA, ROCLINK_FLAG_LISTEN, NODE, 0, 9, 0x20, BME, sizeof(BME));
    CHECK_EQ(roclink_sec_seal(&node, &c, NULL, NULL, f, n, f, sizeof(f)), ROCLINK_E_PERSIST_KEY);
    CHECK_EQ(roclink_sec_seal(&node, &c, NULL, NULL, f, n, f, sizeof(f)), ROCLINK_E_PERSIST_KEY);
    CHECK(roclink_sec_key_pending(&node, ck, &e));
    CHECK_EQ(e, 1);
    CHECK_BYTES(ck, 16, CHAIN_KEY_1);
    CHECK_EQ(roclink_sec_key_persisted(&node, 2), ROCLINK_E_ARG); /* wrong epoch */
    CHECK_EQ(roclink_sec_key_persisted(&node, 1), ROCLINK_OK);
    CHECK(!roclink_sec_key_pending(&node, ck, &e));
    /* Forward secrecy on the node: CK(0) is no longer anywhere in its state. */
    CHECK(!contains_key(&node, sizeof(node), NODE_KEY));

    sn = roclink_sec_seal(&node, &c, NULL, NULL, f, n, f, sizeof(f));
    CHECK_BYTES(f, sn, VS6); /* the first frame of epoch 1, independently generated */

    /* The gateway follows the ratchet and drops its copy of CK(0). */
    CHECK(roclink_sec_open(&gw, &peer, f, (size_t)sn, &info) > 0);
    CHECK_EQ(peer.epoch, 1);
    CHECK_BYTES(peer.chain_key, 16, CHAIN_KEY_1);

    /* A reply in epoch 1, bound to that frame, opens on the node. */
    {
        roclink_fcnt_t gc = counter_at(0x500);
        n = hdr_frame(f, ROCLINK_TYPE_ACK, 0, GW, NODE, 9, 0, NULL, 0);
        sn = roclink_sec_seal(&gw, &gc, &info, &peer, f, n, f, sizeof(f));
        CHECK(sn > 0);
        CHECK_EQ(roclink_sec_open(&node, NULL, f, (size_t)sn, &info), 9);
    }

    /* An epoch-0 frame now: its counter is old and its key is gone. */
    memcpy(f, VS1, sizeof(VS1));
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(VS1), &info), ROCLINK_E_REPLAY);
}

static void test_ratchet_daily_and_limits(void) {
    roclink_sec_t node = node_sec(ROCLINK_SEC_ENC, 8);
    roclink_sec_t gw = gw_sec();
    roclink_sec_t far;
    roclink_sec_config_t cfg;
    roclink_sec_peer_t peer;
    roclink_sec_info_t info;
    roclink_fcnt_t c = counter_at(5);
    uint8_t f[ROCLINK_FRAME_MAX_LEN], ck[16], ck2[16];
    uint32_t e = 0, fc;
    size_t n;
    int sn;

    /* A daily ratchet: jump the counter to the next epoch. */
    CHECK_EQ(roclink_fcnt_next_epoch(&c), ROCLINK_OK);
    CHECK_EQ(c.next, 0x10000L);
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, 0, 1, 1, NULL, 0);
    CHECK_EQ(roclink_sec_seal(&node, &c, NULL, NULL, f, n, f, sizeof(f)), ROCLINK_E_PERSIST_KEY);
    CHECK(roclink_sec_key_pending(&node, ck, &e));
    CHECK_EQ(roclink_sec_key_persisted(&node, e), ROCLINK_OK);
    CHECK_EQ(roclink_sec_seal(&node, &c, NULL, NULL, f, n, f, sizeof(f)), ROCLINK_E_PERSIST);
    roclink_fcnt_persisted(&c, roclink_fcnt_to_persist(&c));
    CHECK(roclink_sec_seal(&node, &c, NULL, NULL, f, n, f, sizeof(f)) > 0);

    /* A counter restored behind the key's epoch skips forward, never back. */
    roclink_sec_config_default(&cfg);
    cfg.net_id = NET;
    cfg.addr = NODE;
    CHECK_EQ(roclink_sec_init_node_at(&far, &cfg, CHAIN_KEY_1, 0, 1), ROCLINK_OK);
    c = counter_at(5);
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, 0, 2, 1, NULL, 0);
    CHECK_EQ(roclink_sec_seal(&far, &c, NULL, NULL, f, n, f, sizeof(f)), ROCLINK_E_PERSIST);
    roclink_fcnt_persisted(&c, roclink_fcnt_to_persist(&c));
    sn = roclink_sec_seal(&far, &c, NULL, NULL, f, n, f, sizeof(f));
    CHECK(sn > 0);
    fc = (uint32_t)f[10] | ((uint32_t)f[11] << 8) | ((uint32_t)f[12] << 16) | ((uint32_t)f[13] << 24);
    CHECK_EQ(fc, 0x10000L);

    /* First contact (open admission) ratchets from epoch 0 at most ROCLINK_MAX_EPOCH_SKIP. */
    CHECK_EQ(roclink_sec_chain_advance(NODE_KEY, NET, NODE, 0, 0, 1000, ck), ROCLINK_OK);
    CHECK_EQ(roclink_sec_chain_advance(ck, NET, NODE, 0, 1000, ROCLINK_MAX_EPOCH_SKIP + 1, ck2),
             ROCLINK_OK);
    CHECK_EQ(roclink_sec_init_node_at(&far, &cfg, ck2, 0, ROCLINK_MAX_EPOCH_SKIP + 1), ROCLINK_OK);
    c = counter_at((ROCLINK_MAX_EPOCH_SKIP + 1u) << ROCLINK_EPOCH_SHIFT);
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, GW, 3, 1, NULL, 0);
    sn = roclink_sec_seal(&far, &c, NULL, NULL, f, n, f, sizeof(f));
    CHECK(sn > 0);
    memset(&peer, 0, sizeof(peer)); /* never seen */
    CHECK_EQ(roclink_sec_open_cost(&gw, &peer, f, (size_t)sn), ROCLINK_E_EPOCH);
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, (size_t)sn, &info), ROCLINK_E_EPOCH);
    CHECK_EQ(peer.state, 0);
}

/* A gateway without the master key: nodes are enrolled one by one. */
static void test_enrolled_gateway(void) {
    roclink_sec_t gw;
    roclink_sec_config_t cfg;
    roclink_sec_peer_t peer;
    roclink_sec_info_t info;
    roclink_fcnt_t gc = counter_at(0x100);
    uint8_t f[ROCLINK_FRAME_MAX_LEN];
    size_t n;
    int sn;
    roclink_sec_config_default(&cfg);
    cfg.net_id = NET;
    cfg.addr = GW;
    CHECK_EQ(roclink_sec_init_gateway(&gw, &cfg, NULL, 0), ROCLINK_OK);
    CHECK(!contains_key(&gw, sizeof(gw), MASTER_KEY));
    memset(&peer, 0, sizeof(peer));

    memcpy(f, VS1, sizeof(VS1));
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(VS1), &info), ROCLINK_E_NOT_ENROLLED);
    CHECK_EQ(roclink_sec_peer_admit(&gw, &peer, NODE, 0), ROCLINK_E_ARG); /* needs the master */

    CHECK_EQ(roclink_sec_peer_enroll(&peer, NODE_KEY, 0, 0), ROCLINK_OK);
    memcpy(f, VS1, sizeof(VS1));
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(VS1), &info), 22);

    /* Its downlink is the same bytes as one from a gateway holding the master. */
    n = hdr_frame(f, ROCLINK_TYPE_ACK, ROCLINK_FLAG_MORE, GW, NODE, 7, 0, NULL, 0);
    sn = roclink_sec_seal(&gw, &gc, &info, &peer, f, n, f, sizeof(f));
    CHECK_BYTES(f, sn, VS3);
}

/* A node `NODE` sealing one DATA frame to any gateway at `epoch`, with its key ratcheted there. */
static int frame_at_epoch(uint32_t epoch, uint8_t *f, size_t cap) {
    roclink_sec_t node;
    roclink_sec_config_t cfg;
    roclink_fcnt_t c = counter_at(epoch << ROCLINK_EPOCH_SHIFT);
    uint8_t ck[16];
    size_t n;
    roclink_sec_config_default(&cfg);
    cfg.net_id = NET;
    cfg.addr = NODE;
    CHECK_EQ(roclink_sec_chain_advance(NODE_KEY, NET, NODE, 0, 0, epoch, ck), ROCLINK_OK);
    CHECK_EQ(roclink_sec_init_node_at(&node, &cfg, ck, 0, epoch), ROCLINK_OK);
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, 0, 1, 1, BME, sizeof(BME));
    return roclink_sec_seal(&node, &c, NULL, NULL, f, n, f, cap);
}

/* Gateways refuse cheaply: admission, the epoch-skip limit, and the cost preview. */
static void test_gateway_fail_fast(void) {
    roclink_sec_t gw;
    roclink_sec_config_t cfg;
    roclink_sec_peer_t peer, before;
    roclink_sec_info_t info;
    uint8_t f[ROCLINK_FRAME_MAX_LEN];
    int n;

    /* Configuration: admission must be set, the skip limit defaults to 32. */
    roclink_sec_config_default(&cfg);
    cfg.net_id = NET;
    cfg.addr = GW;
    CHECK_EQ(cfg.admission, ROCLINK_ADMIT_OPEN);
    cfg.admission = ROCLINK_ADMIT_UNSET;
    CHECK_EQ(roclink_sec_init_gateway(&gw, &cfg, MASTER_KEY, 0), ROCLINK_E_ARG);
    cfg.admission = ROCLINK_ADMIT_OPEN;
    cfg.max_epoch_skip = ROCLINK_MAX_EPOCH_SKIP + 1;
    CHECK_EQ(roclink_sec_init_gateway(&gw, &cfg, MASTER_KEY, 0), ROCLINK_E_ARG);
    cfg.max_epoch_skip = 0;
    CHECK_EQ(roclink_sec_init_gateway(&gw, &cfg, MASTER_KEY, 0), ROCLINK_OK);
    CHECK_EQ(gw.cfg.max_epoch_skip, ROCLINK_EPOCH_SKIP_DEFAULT);

    /* Open admission keeps accepting unknown nodes, even far along: first
     * contact may ratchet up to ROCLINK_MAX_EPOCH_SKIP from epoch 0. */
    memset(&peer, 0, sizeof(peer));
    n = frame_at_epoch(40, f, sizeof(f));
    CHECK_EQ(roclink_sec_open_cost(&gw, &peer, f, (size_t)n), 42); /* CK(0), 40 steps, FK(40) */
    CHECK(roclink_sec_open(&gw, &peer, f, (size_t)n, &info) > 0);
    CHECK_EQ(peer.epoch, 40);

    /* Once known, a node's key moves at most max_epoch_skip per frame. */
    n = frame_at_epoch(40 + 33, f, sizeof(f));
    before = peer;
    CHECK_EQ(roclink_sec_open_cost(&gw, &peer, f, (size_t)n), ROCLINK_E_EPOCH);
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, (size_t)n, &info), ROCLINK_E_EPOCH);
    CHECK(memcmp(&before, &peer, sizeof(peer)) == 0); /* nothing changed */
    n = frame_at_epoch(40 + 32, f, sizeof(f));
    CHECK_EQ(roclink_sec_open_cost(&gw, &peer, f, (size_t)n), 33);
    CHECK(roclink_sec_open(&gw, &peer, f, (size_t)n, &info) > 0);

    /* The current epoch costs nothing but the CCM check, thanks to the
     * cached frame key; a forged frame there is refused by its tag. */
    n = frame_at_epoch(72, f, sizeof(f));
    f[10] = 0x05; /* a fresh counter in the same epoch, which breaks the tag */
    CHECK_EQ(roclink_sec_open_cost(&gw, &peer, f, (size_t)n), 0);
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, (size_t)n, &info), ROCLINK_E_AUTH);
    n = frame_at_epoch(73, f, sizeof(f)); /* the daily ratchet: one step and FK */
    CHECK_EQ(roclink_sec_open_cost(&gw, &peer, f, (size_t)n), 2);

    /* Cheap refusals report their status without any derivation. */
    memcpy(f, VS1, sizeof(VS1));
    f[2] = NET + 1;
    CHECK_EQ(roclink_sec_open_cost(&gw, &peer, f, sizeof(VS1)), ROCLINK_E_NET);
    CHECK_EQ(roclink_sec_open_cost(NULL, &peer, VS1, sizeof(VS1)), ROCLINK_E_ARG);

    /* Listed admission: a master-mode gateway accepts only nodes it was told about. */
    cfg.admission = ROCLINK_ADMIT_LISTED;
    CHECK_EQ(roclink_sec_init_gateway(&gw, &cfg, MASTER_KEY, 0), ROCLINK_OK);
    memset(&peer, 0, sizeof(peer));
    memcpy(f, VS1, sizeof(VS1));
    CHECK_EQ(roclink_sec_open_cost(&gw, &peer, f, sizeof(VS1)), ROCLINK_E_NOT_ENROLLED);
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(VS1), &info), ROCLINK_E_NOT_ENROLLED);
    CHECK_EQ(peer.state, 0);
    CHECK_EQ(roclink_sec_peer_admit(&gw, &peer, NODE, 0), ROCLINK_OK);
    CHECK_BYTES(peer.chain_key, 16, NODE_KEY);
    CHECK_EQ(roclink_sec_open_cost(&gw, &peer, f, sizeof(VS1)), 0); /* FK(0) cached at admission */
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(VS1), &info), 22);
    CHECK_EQ(roclink_sec_peer_admit(&gw, &peer, ROCLINK_ADDR_GATEWAY_MIN, 0), ROCLINK_E_ARG);
}

static void test_revocation(void) {
    roclink_sec_t gw = gw_sec();
    roclink_sec_peer_t peer;
    roclink_sec_info_t info, opener;
    roclink_fcnt_t gc = counter_at(0x100);
    uint8_t f[ROCLINK_FRAME_MAX_LEN];
    size_t n;
    memset(&peer, 0, sizeof(peer));
    peer.policy = ROCLINK_POLICY_DENY;

    memcpy(f, VS1, sizeof(VS1));
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(VS1), &info), ROCLINK_E_REVOKED);
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, GW, 1, 1, NULL, 0); /* plaintext too */
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, n, &info), ROCLINK_E_REVOKED);
    memset(&opener, 0, sizeof(opener));
    opener.level = ROCLINK_SEC_ENC;
    opener.fcnt = 1;
    n = hdr_frame(f, ROCLINK_TYPE_ACK, 0, GW, NODE, 1, 0, NULL, 0);
    CHECK_EQ(roclink_sec_seal(&gw, &gc, &opener, &peer, f, n, f, sizeof(f)), ROCLINK_E_REVOKED);
    CHECK_EQ(peer.state, 0);

    roclink_sec_config_t cfg; /* DENY is a per-node override, not a network policy */
    roclink_sec_t other;
    roclink_sec_config_default(&cfg);
    cfg.net_id = NET;
    cfg.addr = GW;
    cfg.policy = ROCLINK_POLICY_DENY;
    CHECK_EQ(roclink_sec_init_gateway(&other, &cfg, MASTER_KEY, 0), ROCLINK_E_ARG);
}

/* Two devices holding the same key and counter: a clone, or a counter reset. */
static void test_clone_conflict(void) {
    roclink_sec_t a = node_sec(ROCLINK_SEC_ENC, 8);
    roclink_sec_t b = node_sec(ROCLINK_SEC_ENC, 8);
    roclink_sec_t gw = gw_sec();
    roclink_sec_peer_t peer;
    roclink_sec_info_t info;
    roclink_fcnt_t ca = counter_at(7), cb = counter_at(7);
    uint8_t fa[64], fb[64], g[64], tag_before[8];
    size_t n;
    int na, nb;
    memset(&peer, 0, sizeof(peer));

    n = hdr_frame(fa, ROCLINK_TYPE_DATA, 0, NODE, GW, 1, 1, (const uint8_t *)"a", 1);
    na = roclink_sec_seal(&a, &ca, NULL, NULL, fa, n, fa, sizeof(fa));
    n = hdr_frame(fb, ROCLINK_TYPE_DATA, 0, NODE, GW, 1, 1, (const uint8_t *)"b", 1);
    nb = roclink_sec_seal(&b, &cb, NULL, NULL, fb, n, fb, sizeof(fb));
    CHECK(na > 0 && nb > 0);

    memcpy(g, fa, (size_t)na);
    CHECK(roclink_sec_open(&gw, &peer, g, (size_t)na, &info) > 0);
    memcpy(g, fa, (size_t)na); /* the same frame again: a normal retransmission */
    CHECK(roclink_sec_open(&gw, &peer, g, (size_t)na, &info) > 0);
    CHECK(info.retransmission);
    memcpy(tag_before, peer.last_tag, 8);
    memcpy(g, fb, (size_t)nb); /* authentic, same counter, different content */
    CHECK_EQ(roclink_sec_open(&gw, &peer, g, (size_t)nb, &info), ROCLINK_E_CONFLICT);
    CHECK_EQ(peer.last_fcnt, 7);
    CHECK(memcmp(peer.last_tag, tag_before, 8) == 0); /* no state change */
}

/* ------------------------------------------------- configuration, misuse */

static void test_configuration(void) {
    roclink_sec_t s;
    roclink_sec_config_t cfg;
    roclink_sec_info_t info, opener;
    roclink_fcnt_t c = counter_at(0);
    uint8_t f[ROCLINK_FRAME_MAX_LEN], big[ROCLINK_FRAME_MAX_LEN];
    roclink_rx_t rx;
    roclink_client_t client;
    roclink_client_config_t cc;
    size_t n;

    memset(&cfg, 0, sizeof(cfg)); /* zero-initialised: no silent "insecure" */
    cfg.net_id = NET;
    cfg.addr = NODE;
    cfg.tag_len = 8;
    CHECK_EQ(roclink_sec_init_node(&s, &cfg, NODE_KEY, 0), ROCLINK_E_ARG);
    roclink_sec_config_default(&cfg);
    cfg.net_id = NET;
    cfg.addr = NODE;
    CHECK_EQ(roclink_sec_init_node(&s, &cfg, NULL, 0), ROCLINK_E_ARG); /* enc needs a key */
    cfg.tag_len = 6;
    CHECK_EQ(roclink_sec_init_node(&s, &cfg, NODE_KEY, 0), ROCLINK_E_ARG);
    cfg.tag_len = 8;
    cfg.addr = GW;
    CHECK_EQ(roclink_sec_init_node(&s, &cfg, NODE_KEY, 0), ROCLINK_E_ARG); /* role vs address */

    cfg.addr = NODE;
    cfg.min_tag_len = 6;
    CHECK_EQ(roclink_sec_init_node(&s, &cfg, NODE_KEY, 0), ROCLINK_E_ARG);
    cfg.min_tag_len = 0; /* the default: 8 */
    CHECK_EQ(roclink_sec_init_node(&s, &cfg, NODE_KEY, 0), ROCLINK_OK);
    CHECK_EQ(s.cfg.min_tag_len, 8);
    /* A secured node can't accept unsecured gateway frames (PROTOCOL.md 10.1). */
    cfg.policy = ROCLINK_POLICY_ALLOW_NONE;
    CHECK_EQ(roclink_sec_init_node(&s, &cfg, NODE_KEY, 0), ROCLINK_E_ARG);

    /* The explicit opt-out: no key, level none, allow-none. */
    cfg.level = ROCLINK_SEC_NONE;
    CHECK_EQ(roclink_sec_init_node(&s, &cfg, NULL, 0), ROCLINK_OK);
    n = hdr_frame(f, ROCLINK_TYPE_DATA, ROCLINK_FLAG_LISTEN, NODE, 0, 1, 1, NULL, 0);
    CHECK_EQ(roclink_sec_seal(&s, &c, NULL, NULL, f, n, f, sizeof(f)), (int)n); /* passes through */
    CHECK_EQ(f[1] & ROCLINK_FLAG_SECURED, 0);

    /* Size limits. */
    s = node_sec(ROCLINK_SEC_ENC, 8);
    memset(big, 0, sizeof(big));
    /* The largest payload that still fits once sealed: 9 + 5 + payload + 8 = the build's limit. */
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, 0, 1, 1, big, ROCLINK_FRAME_MAX_LEN - 22);
    CHECK_EQ(roclink_sec_seal(&s, &c, NULL, NULL, f, n, f, sizeof(f)), (int)ROCLINK_FRAME_MAX_LEN);
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, 0, 1, 1, big, ROCLINK_FRAME_MAX_LEN - 21);
    CHECK_EQ(roclink_sec_seal(&s, &c, NULL, NULL, f, n, f, sizeof(f)), ROCLINK_E_TOO_LONG);
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, 0, 1, 1, big, 10);
    CHECK_EQ(roclink_sec_seal(&s, &c, NULL, NULL, f, n, f, n + 12), ROCLINK_E_NOSPACE);
    CHECK_EQ(roclink_sec_seal(&s, &c, NULL, NULL, VS1, sizeof(VS1), f, sizeof(f)), ROCLINK_E_ARG);
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, 0x4321, 0, 1, 1, NULL, 0); /* not our address */
    CHECK_EQ(roclink_sec_seal(&s, &c, NULL, NULL, f, n, f, sizeof(f)), ROCLINK_E_ADDR);

    /* Gateway frames are replies: an opener is required, and an unsecured
     * opener gets an unsecured reply. */
    s = gw_sec();
    n = hdr_frame(f, ROCLINK_TYPE_ACK, 0, GW, NODE, 1, 0, NULL, 0);
    CHECK_EQ(roclink_sec_seal(&s, &c, NULL, NULL, f, n, f, sizeof(f)), ROCLINK_E_ARG);
    memset(&opener, 0, sizeof(opener));
    opener.level = ROCLINK_SEC_NONE;
    CHECK_EQ(roclink_sec_seal(&s, &c, &opener, NULL, f, n, f, sizeof(f)), (int)n);

    /* Client and server refuse frames that were never opened. */
    cc.net_id = NET;
    cc.node_id = NODE;
    cc.gateway_id = ROCLINK_ADDR_ANY_GATEWAY;
    CHECK_EQ(roclink_client_init(&client, &cc, 0), ROCLINK_OK);
    roclink_client_handle(&client, VS4, sizeof(VS4), &rx);
    CHECK_EQ(rx.status, ROCLINK_E_POLICY);
    (void)info;
}

/* A gateway may allow `none` network-wide only on an open net (PROTOCOL.md 10.1, Annex B). */
static void test_open_net(void) {
    roclink_sec_t gw;
    roclink_sec_config_t cfg;
    roclink_sec_peer_t peer;
    roclink_sec_info_t info;
    uint8_t f[ROCLINK_FRAME_MAX_LEN];
    size_t n;
    roclink_sec_config_default(&cfg);
    cfg.net_id = NET;
    cfg.addr = GW;
    cfg.policy = ROCLINK_POLICY_ALLOW_NONE;
    CHECK_EQ(roclink_sec_init_gateway(&gw, &cfg, MASTER_KEY, 0), ROCLINK_E_ARG); /* secured net */
    cfg.open_net = true;
    cfg.level = ROCLINK_SEC_NONE;
    CHECK_EQ(roclink_sec_init_gateway(&gw, &cfg, NULL, 0), ROCLINK_OK); /* no keys at all */
    memset(&peer, 0, sizeof(peer));
    n = hdr_frame(f, ROCLINK_TYPE_DATA, ROCLINK_FLAG_LISTEN, NODE, GW, 1, 1, NULL, 0);
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, n, &info), (int)n);
    CHECK_EQ(info.level, ROCLINK_SEC_NONE);
    memcpy(f, VS1, sizeof(VS1)); /* a secured frame: nobody here holds its key */
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(VS1), &info), ROCLINK_E_NOT_ENROLLED);
}

/* The node tracks each gateway's counter separately within one reply. */
static void test_reply_per_gateway(void) {
    roclink_sec_t node = node_sec(ROCLINK_SEC_ENC, 8);
    roclink_sec_t gws[ROCLINK_SEC_REPLY_GATEWAYS + 1];
    roclink_sec_config_t cfg;
    roclink_sec_peer_t peer;
    roclink_sec_info_t info, opener;
    roclink_fcnt_t nctr = counter_at(1), gctr;
    uint8_t f[ROCLINK_FRAME_MAX_LEN], a[64], b[64];
    size_t n;
    int na, nb, i;

    roclink_sec_config_default(&cfg);
    cfg.net_id = NET;
    for (i = 0; i <= (int)ROCLINK_SEC_REPLY_GATEWAYS; ++i) {
        cfg.addr = (uint16_t)(GW + i);
        CHECK_EQ(roclink_sec_init_gateway(&gws[i], &cfg, MASTER_KEY, 0), ROCLINK_OK);
    }
    n = hdr_frame(f, ROCLINK_TYPE_DATA, ROCLINK_FLAG_LISTEN, NODE, 0, 1, 1, NULL, 0);
    CHECK(roclink_sec_seal(&node, &nctr, NULL, NULL, f, n, f, sizeof(f)) > 0); /* fcnt 1 */
    memset(&opener, 0, sizeof(opener));
    opener.level = ROCLINK_SEC_ENC;
    opener.fcnt = 1;

    /* Gateway A at counter 0x300, then gateway B at a lower 0x10: both fine. */
    memset(&peer, 0, sizeof(peer));
    gctr = counter_at(0x300);
    n = hdr_frame(f, ROCLINK_TYPE_ACK, 0, GW, NODE, 1, 0, NULL, 0);
    na = roclink_sec_seal(&gws[0], &gctr, &opener, &peer, f, n, a, sizeof(a));
    gctr = counter_at(0x10);
    n = hdr_frame(f, ROCLINK_TYPE_ACK, 0, GW + 1, NODE, 1, 0, NULL, 0);
    nb = roclink_sec_seal(&gws[1], &gctr, &opener, &peer, f, n, b, sizeof(b));
    memcpy(f, a, (size_t)na);
    CHECK_EQ(roclink_sec_open(&node, NULL, f, (size_t)na, &info), 9);
    memcpy(f, b, (size_t)nb);
    CHECK_EQ(roclink_sec_open(&node, NULL, f, (size_t)nb, &info), 9);
    /* Each one's recording is now a replay. */
    memcpy(f, a, (size_t)na);
    CHECK_EQ(roclink_sec_open(&node, NULL, f, (size_t)na, &info), ROCLINK_E_REPLAY);
    memcpy(f, b, (size_t)nb);
    CHECK_EQ(roclink_sec_open(&node, NULL, f, (size_t)nb, &info), ROCLINK_E_REPLAY);

    /* Room for ROCLINK_SEC_REPLY_GATEWAYS gateways per reply; one more is refused. */
    for (i = 2; i <= (int)ROCLINK_SEC_REPLY_GATEWAYS; ++i) {
        gctr = counter_at(0x400);
        n = hdr_frame(f, ROCLINK_TYPE_ACK, 0, (uint16_t)(GW + i), NODE, 1, 0, NULL, 0);
        na = roclink_sec_seal(&gws[i], &gctr, &opener, &peer, f, n, f, sizeof(f));
        CHECK_EQ(roclink_sec_open(&node, NULL, f, (size_t)na, &info),
                 i < (int)ROCLINK_SEC_REPLY_GATEWAYS ? 9 : ROCLINK_E_UNEXPECTED);
    }
}

/* An always-listening node accepts unsolicited downlink bound to its latest uplink. */
static void test_always_listening(void) {
    roclink_sec_t node, gw = gw_sec();
    roclink_sec_config_t cfg;
    roclink_sec_peer_t peer;
    roclink_sec_info_t info, binding;
    roclink_fcnt_t nctr = counter_at(1), gctr = counter_at(0x200);
    uint8_t f[ROCLINK_FRAME_MAX_LEN], dl[64];
    const uint8_t cmd[] = {0x01};
    size_t n;
    int dn;

    roclink_sec_config_default(&cfg);
    cfg.net_id = NET;
    cfg.addr = NODE;
    cfg.always_listening = true;
    CHECK_EQ(roclink_sec_init_node(&node, &cfg, NODE_KEY, 0), ROCLINK_OK);
    memset(&peer, 0, sizeof(peer));

    /* After boot it accepts nothing until it has sent an uplink. */
    memcpy(f, VS4, sizeof(VS4));
    CHECK_EQ(roclink_sec_open(&node, NULL, f, sizeof(VS4), &info), ROCLINK_E_UNEXPECTED);

    /* A plain fire-and-forget uplink (no LISTEN) still binds it, at fcnt 1. */
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, 0, 1, 1, NULL, 0);
    CHECK(roclink_sec_seal(&node, &nctr, NULL, NULL, f, n, f, sizeof(f)) > 0);
    roclink_sec_close_window(&node); /* its window never closes */

    /* The hub sends "open" at once, bound to the latest uplink it has seen. */
    memset(&binding, 0, sizeof(binding));
    binding.level = ROCLINK_SEC_ENC;
    binding.fcnt = 1;
    n = hdr_frame(f, ROCLINK_TYPE_DATA, ROCLINK_FLAG_ACK_REQ, GW, NODE, 0x40, 10, cmd, 1);
    dn = roclink_sec_seal(&gw, &gctr, &binding, &peer, f, n, dl, sizeof(dl));
    CHECK(dn > 0);
    memcpy(f, dl, (size_t)dn);
    CHECK_EQ(roclink_sec_open(&node, NULL, f, (size_t)dn, &info), 10);
    memcpy(f, dl, (size_t)dn);
    CHECK_EQ(roclink_sec_open(&node, NULL, f, (size_t)dn, &info), ROCLINK_E_REPLAY);

    /* The next uplink moves the binding: the recording no longer verifies. */
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, 0, 2, 1, NULL, 0);
    CHECK(roclink_sec_seal(&node, &nctr, NULL, NULL, f, n, f, sizeof(f)) > 0);
    memcpy(f, dl, (size_t)dn);
    CHECK_EQ(roclink_sec_open(&node, NULL, f, (size_t)dn, &info), ROCLINK_E_AUTH);
}

static void test_peer_record(void) {
    roclink_sec_t gw = gw_sec();
    roclink_sec_peer_t peer, back;
    roclink_sec_info_t info;
    uint8_t f[sizeof(VS1)], rec[ROCLINK_SEC_PEER_RECORD_LEN];
    memset(&peer, 0, sizeof(peer));
    peer.policy = ROCLINK_POLICY_REQUIRE_AUTH;
    peer.min_tag = 8;
    memcpy(f, VS1, sizeof(VS1));
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, sizeof(VS1), &info), 22);
    roclink_sec_peer_save(&peer, rec);
    CHECK_EQ(rec[0], 1);
    CHECK_EQ(rec[1], 1); /* last_fcnt, little-endian */
    CHECK_EQ(roclink_sec_peer_load(&back, rec), ROCLINK_OK);
    CHECK(memcmp(&back, &peer, sizeof(peer)) == 0);
    /* A restored gateway still refuses the replay. */
    memcpy(f, VS1, sizeof(VS1));
    CHECK(roclink_sec_open(&gw, &back, f, sizeof(VS1), &info) > 0);
    CHECK(info.retransmission);
    rec[0] = 2;
    CHECK_EQ(roclink_sec_peer_load(&back, rec), ROCLINK_E_ARG);
}

/* Jumbo frames: enc works at any size the build allows; auth stays within 255 B. */
static void test_jumbo_limits(void) {
#if defined(ROCLINK_JUMBO)
    static uint8_t f[ROCLINK_FRAME_MAX_LEN], g[ROCLINK_FRAME_MAX_LEN], big[ROCLINK_FRAME_MAX_LEN];
    roclink_sec_t node = node_sec(ROCLINK_SEC_ENC, 8);
    roclink_sec_t node_auth = node_sec(ROCLINK_SEC_AUTH, 8);
    roclink_sec_t gw = gw_sec();
    roclink_sec_peer_t peer;
    roclink_sec_info_t info;
    roclink_fcnt_t c = counter_at(1);
    size_t i, n;
    int sn;
    for (i = 0; i < sizeof(big); ++i) big[i] = (uint8_t)(i * 7u);
    memset(&peer, 0, sizeof(peer));

    /* A 1000-byte payload, encrypted, survives the round trip. */
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, GW, 1, 1, big, 1000);
    sn = roclink_sec_seal(&node, &c, NULL, NULL, f, n, f, sizeof(f));
    CHECK_EQ(sn, 1022);
    memcpy(g, f, (size_t)sn);
    CHECK_EQ(roclink_sec_open(&gw, &peer, g, (size_t)sn, &info), 1009);
    CHECK(memcmp(g + 9, big, 1000) == 0);
    memcpy(g, f, (size_t)sn);
    g[700] ^= 1; /* tamper deep inside the jumbo ciphertext */
    memset(&peer, 0, sizeof(peer));
    CHECK_EQ(roclink_sec_open(&gw, &peer, g, (size_t)sn, &info), ROCLINK_E_AUTH);

    /* auth is capped at the standard 255 B frame in every build. */
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, GW, 2, 1, big, 233); /* sealed: exactly 255 */
    CHECK_EQ(roclink_sec_seal(&node_auth, &c, NULL, NULL, f, n, f, sizeof(f)), 255);
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, GW, 3, 1, big, 234);
    CHECK_EQ(roclink_sec_seal(&node_auth, &c, NULL, NULL, f, n, f, sizeof(f)), ROCLINK_E_TOO_LONG);
    /* A received auth frame over 255 B is refused before any crypto. */
    n = hdr_frame(f, ROCLINK_TYPE_DATA, 0, NODE, GW, 4, 1, big, 400);
    sn = roclink_sec_seal(&node, &c, NULL, NULL, f, n, f, sizeof(f));
    f[9] = (uint8_t)(f[9] & ~ROCLINK_SECCTRL_ENC); /* pretend it's auth */
    memset(&peer, 0, sizeof(peer));
    peer.policy = ROCLINK_POLICY_REQUIRE_AUTH; /* past the policy check, to reach the size cap */
    CHECK_EQ(roclink_sec_open(&gw, &peer, f, (size_t)sn, &info), ROCLINK_E_TOO_LONG);
#else
    CHECK_EQ(ROCLINK_FRAME_MAX_LEN, 255); /* no jumbo: nothing to test beyond the limit */
#endif
}

int main(void) {
    RUN(test_primitives);
    RUN(test_key_derivation);
    RUN(test_frame_counter);
    RUN(test_seal_matches_vectors);
    RUN(test_secured_exchange);
    RUN(test_secured_dedupe_ignores_seq);
    RUN(test_tampering_is_rejected);
    RUN(test_replay_and_keys);
    RUN(test_key_rotation);
    RUN(test_policy_and_downgrade);
    RUN(test_downlink_replay_after_new_uplink);
    RUN(test_close_window);
    RUN(test_ratchet_epoch_change);
    RUN(test_ratchet_daily_and_limits);
    RUN(test_enrolled_gateway);
    RUN(test_gateway_fail_fast);
    RUN(test_revocation);
    RUN(test_clone_conflict);
    RUN(test_configuration);
    RUN(test_open_net);
    RUN(test_reply_per_gateway);
    RUN(test_always_listening);
    RUN(test_peer_record);
    RUN(test_jumbo_limits);
    return REPORT();
}
