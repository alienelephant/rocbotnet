/* Frame-format tests. The golden vectors are duplicated in
 * docs/PROTOCOL.md section 11; any other implementation must reproduce them. */
#include "roclink.h"

#include "check.h"

static const uint8_t V1_UPLINK_ACK[] = {0x20, 0x03, 0x42, 0x34, 0x12, 0x00, 0x00, 0x07, 0x01, 0x68, 0x69};
static const uint8_t V2_GW_ACK_MORE[] = {0x24, 0x04, 0x42, 0x01, 0xFF, 0x34, 0x12, 0x07, 0x00};
static const uint8_t V3_DOWNLINK[] = {0x20, 0x09, 0x42, 0x01, 0xFF, 0x34, 0x12, 0x90, 0x0A, 0x3C, 0x00};
static const uint8_t V4_NODE_ACK[] = {0x24, 0x00, 0x42, 0x34, 0x12, 0x01, 0xFF, 0x90, 0x00};
static const uint8_t V5_RELAYED[] = {0x20, 0x30, 0x42, 0x34, 0x12, 0x00, 0x00, 0x08, 0x03, 0xEF, 0xBE, 0xAA};

static roclink_header_t hdr(uint8_t type, uint8_t flags, uint16_t src, uint16_t dst, uint8_t seq,
                            uint8_t schema) {
    roclink_header_t h;
    h.type = type;
    h.flags = flags;
    h.net_id = 0x42;
    h.src = src;
    h.dst = dst;
    h.seq = seq;
    h.schema = schema;
    h.relay = 0;
    return h;
}

static void test_golden_vectors_encode(void) {
    uint8_t out[ROCLINK_FRAME_MAX_LEN];
    roclink_header_t h;
    const uint8_t hi[] = {'h', 'i'};
    const uint8_t interval[] = {0x3C, 0x00};
    const uint8_t aa[] = {0xAA};
    int n;

    h = hdr(ROCLINK_TYPE_DATA, ROCLINK_FLAG_ACK_REQ | ROCLINK_FLAG_LISTEN, 0x1234,
            ROCLINK_ADDR_ANY_GATEWAY, 7, 1);
    n = roclink_encode(&h, hi, sizeof(hi), out, sizeof(out));
    CHECK_BYTES(out, n, V1_UPLINK_ACK);

    h = hdr(ROCLINK_TYPE_ACK, ROCLINK_FLAG_MORE, 0xFF01, 0x1234, 7, ROCLINK_SCHEMA_CONTROL);
    n = roclink_encode(&h, NULL, 0, out, sizeof(out));
    CHECK_BYTES(out, n, V2_GW_ACK_MORE);

    h = hdr(ROCLINK_TYPE_DATA, ROCLINK_FLAG_ACK_REQ | ROCLINK_FLAG_DL_QUEUED, 0xFF01, 0x1234,
            0x90, 10);
    n = roclink_encode(&h, interval, sizeof(interval), out, sizeof(out));
    CHECK_BYTES(out, n, V3_DOWNLINK);

    h = hdr(ROCLINK_TYPE_ACK, 0, 0x1234, 0xFF01, 0x90, ROCLINK_SCHEMA_CONTROL);
    n = roclink_encode(&h, NULL, 0, out, sizeof(out));
    CHECK_BYTES(out, n, V4_NODE_ACK);

    h = hdr(ROCLINK_TYPE_DATA, ROCLINK_FLAG_RELAYED | (1u << ROCLINK_FLAG_HOPS_SHIFT), 0x1234,
            ROCLINK_ADDR_ANY_GATEWAY, 8, 3);
    h.relay = 0xBEEF;
    n = roclink_encode(&h, aa, sizeof(aa), out, sizeof(out));
    CHECK_BYTES(out, n, V5_RELAYED);
}

static void test_golden_vectors_decode(void) {
    roclink_header_t h;
    const uint8_t *p = NULL;
    size_t plen = 0;

    CHECK_EQ(roclink_decode(V1_UPLINK_ACK, sizeof(V1_UPLINK_ACK), &h, &p, &plen), ROCLINK_OK);
    CHECK_EQ(h.type, ROCLINK_TYPE_DATA);
    CHECK_EQ(h.flags, ROCLINK_FLAG_ACK_REQ | ROCLINK_FLAG_LISTEN);
    CHECK_EQ(h.net_id, 0x42);
    CHECK_EQ(h.src, 0x1234);
    CHECK_EQ(h.dst, ROCLINK_ADDR_ANY_GATEWAY);
    CHECK_EQ(h.seq, 7);
    CHECK_EQ(h.schema, 1);
    CHECK_EQ(plen, 2);
    CHECK(p == V1_UPLINK_ACK + ROCLINK_HEADER_LEN);

    CHECK_EQ(roclink_decode(V2_GW_ACK_MORE, sizeof(V2_GW_ACK_MORE), &h, &p, &plen), ROCLINK_OK);
    CHECK_EQ(h.type, ROCLINK_TYPE_ACK);
    CHECK_EQ(h.src, 0xFF01);
    CHECK_EQ(plen, 0);

    CHECK_EQ(roclink_decode(V5_RELAYED, sizeof(V5_RELAYED), &h, &p, &plen), ROCLINK_OK);
    CHECK_EQ(h.relay, 0xBEEF);
    CHECK_EQ((h.flags & ROCLINK_FLAG_HOPS_MASK) >> ROCLINK_FLAG_HOPS_SHIFT, 1);
    CHECK_EQ(plen, 1);
    CHECK_EQ(p[0], 0xAA);
}

static void test_decode_rejects(void) {
    roclink_header_t h;
    const uint8_t *p = NULL;
    size_t plen = 0;
    const uint8_t short_frame[] = {0x20, 0x00, 0x42, 0x34, 0x12, 0x00, 0x00, 0x07};
    const uint8_t bad_version[] = {0x40, 0x00, 0x42, 0x34, 0x12, 0x00, 0x00, 0x07, 0x01};
    const uint8_t bad_type[] = {0x28, 0x00, 0x42, 0x34, 0x12, 0x00, 0x00, 0x07, 0x01};
    const uint8_t truncated_relay[] = {0x20, 0x10, 0x42, 0x34, 0x12, 0x00, 0x00, 0x07, 0x01, 0xEF};
    const uint8_t reserved_bits[] = {0x23, 0x00, 0x42, 0x34, 0x12, 0x00, 0x00, 0x07, 0x01};
    /* SECURED with no room for the 5-byte security header and the tag. */
    const uint8_t secured_short[] = {0x20, 0x80, 0x42, 0x34, 0x12, 0x00, 0x00, 0x07, 0x01,
                                     0x30, 1, 0, 0, 0, 0xAA, 0xBB, 0xCC};

    CHECK_EQ(roclink_decode(short_frame, sizeof(short_frame), &h, &p, &plen), ROCLINK_E_SHORT);
    CHECK_EQ(roclink_decode(bad_version, sizeof(bad_version), &h, &p, &plen), ROCLINK_E_VERSION);
    CHECK_EQ(roclink_decode(bad_type, sizeof(bad_type), &h, &p, &plen), ROCLINK_E_TYPE);
    CHECK_EQ(roclink_decode(truncated_relay, sizeof(truncated_relay), &h, &p, &plen),
             ROCLINK_E_SHORT);
    CHECK_EQ(roclink_decode(NULL, 9, &h, &p, &plen), ROCLINK_E_ARG);
    /* Reserved bits are ignored on receive (forward compatibility). */
    CHECK_EQ(roclink_decode(reserved_bits, sizeof(reserved_bits), &h, &p, &plen), ROCLINK_OK);
    CHECK_EQ(roclink_decode(secured_short, 9, &h, &p, &plen), ROCLINK_E_SHORT);
    CHECK_EQ(roclink_decode(secured_short, sizeof(secured_short), &h, &p, &plen),
             ROCLINK_E_SHORT); /* 8-byte tag (sec_ctrl 0x30) doesn't fit */
}

static void test_decode_rejects_malformed_ack(void) {
    roclink_header_t h;
    const uint8_t *p = NULL;
    size_t plen = 0;
    /* Vector 2 (an ACK) with one payload byte, and with schema 1. */
    const uint8_t ack_payload[] = {0x24, 0x04, 0x42, 0x01, 0xFF, 0x34, 0x12, 0x07, 0x00, 0xAA};
    const uint8_t ack_port[] = {0x24, 0x04, 0x42, 0x01, 0xFF, 0x34, 0x12, 0x07, 0x01};
    /* A secured ACK: header | sec_ctrl 0x20 (enc, 4-byte tag) | fcnt 1 | 1 payload byte | tag */
    const uint8_t sec_ack_payload[] = {0x24, 0x80, 0x42, 0x01, 0xFF, 0x34, 0x12, 0x07, 0x00, 0x20,
                                       0x01, 0x00, 0x00, 0x00, 0xAA, 1, 2, 3, 4};
    const uint8_t sec_ack_ok[] = {0x24, 0x80, 0x42, 0x01, 0xFF, 0x34, 0x12, 0x07, 0x00, 0x20,
                                  0x01, 0x00, 0x00, 0x00, 1, 2, 3, 4};

    CHECK_EQ(roclink_decode(V2_GW_ACK_MORE, sizeof(V2_GW_ACK_MORE), &h, &p, &plen), ROCLINK_OK);
    CHECK_EQ(roclink_decode(ack_payload, sizeof(ack_payload), &h, &p, &plen), ROCLINK_E_MALFORMED);
    CHECK_EQ(roclink_decode(ack_port, sizeof(ack_port), &h, &p, &plen), ROCLINK_E_MALFORMED);
    CHECK_EQ(roclink_decode(sec_ack_payload, sizeof(sec_ack_payload), &h, &p, &plen),
             ROCLINK_E_MALFORMED);
    CHECK_EQ(roclink_decode(sec_ack_ok, sizeof(sec_ack_ok), &h, &p, &plen), ROCLINK_OK);
    CHECK_EQ(plen, 0);
}

static void test_decode_secured_layout(void) {
    /* header | sec_ctrl 0x31 (enc, tag 8, gen 1) | fcnt 0x01020304 | 2 payload bytes | 8 tag bytes */
    const uint8_t f[] = {0x20, 0x80, 0x42, 0x34, 0x12, 0x00, 0x00, 0x07, 0x01, 0x31,
                         0x04, 0x03, 0x02, 0x01, 0xAA, 0xBB, 1, 2, 3, 4, 5, 6, 7, 8};
    roclink_header_t h;
    const uint8_t *p = NULL;
    size_t plen = 0;
    CHECK_EQ(roclink_decode(f, sizeof(f), &h, &p, &plen), ROCLINK_OK);
    CHECK_EQ(h.sec_ctrl, 0x31);
    CHECK_EQ(h.fcnt, 0x01020304L);
    CHECK_EQ(plen, 2);
    CHECK(p == f + 14);
    CHECK_EQ(roclink_sec_tag_len(0x31), 8);
    CHECK_EQ(roclink_sec_tag_len(0x21), 4);
}

static void test_encode_rejects(void) {
    uint8_t out[ROCLINK_FRAME_MAX_LEN];
    uint8_t big[ROCLINK_PAYLOAD_MAX + 1];
    roclink_header_t h = hdr(ROCLINK_TYPE_DATA, 0, 0x1234, 0, 1, 1);
    memset(big, 0, sizeof(big));

    CHECK_EQ(roclink_encode(&h, big, ROCLINK_PAYLOAD_MAX, out, sizeof(out)),
             ROCLINK_FRAME_MAX_LEN);
    CHECK_EQ(roclink_encode(&h, big, sizeof(big), out, sizeof(out)), ROCLINK_E_TOO_LONG);
    CHECK_EQ(roclink_encode(&h, big, 10, out, 18), ROCLINK_E_NOSPACE);
    CHECK_EQ(roclink_encode(&h, NULL, 1, out, sizeof(out)), ROCLINK_E_ARG);

    h.flags = ROCLINK_FLAG_SECURED; /* only roclink_sec_seal() makes secured frames */
    CHECK_EQ(roclink_encode(&h, NULL, 0, out, sizeof(out)), ROCLINK_E_ARG);
    h.flags = ROCLINK_FLAG_RELAYED; /* 11-byte header leaves 244 bytes */
    CHECK_EQ(roclink_encode(&h, big, ROCLINK_PAYLOAD_MAX, out, sizeof(out)), ROCLINK_E_TOO_LONG);

    h = hdr(2, 0, 0x1234, 0, 1, 1);
    CHECK_EQ(roclink_encode(&h, NULL, 0, out, sizeof(out)), ROCLINK_E_ARG);
    h = hdr(ROCLINK_TYPE_ACK, 0, 0x1234, 0, 1, 1); /* ACK must use schema 0 */
    CHECK_EQ(roclink_encode(&h, NULL, 0, out, sizeof(out)), ROCLINK_E_ARG);
    h.schema = 0;
    CHECK_EQ(roclink_encode(&h, big, 1, out, sizeof(out)), ROCLINK_E_ARG); /* ...and no payload */
}

static void test_addresses(void) {
    const uint8_t mac[] = {0x24, 0x6F, 0x28, 0x11, 0x22, 0x33};
    const uint8_t mac2[] = {0x24, 0x6F, 0x28, 0x11, 0x22, 0x34};
    uint16_t a = roclink_node_id_from_bytes(mac, sizeof(mac));
    uint32_t i;

    CHECK(roclink_is_node_addr(a));
    CHECK_EQ(a, roclink_node_id_from_bytes(mac, sizeof(mac)));
    CHECK(a != roclink_node_id_from_bytes(mac2, sizeof(mac2)));
    for (i = 0; i < 5000; ++i) {
        uint8_t b[4];
        b[0] = (uint8_t)i;
        b[1] = (uint8_t)(i >> 8);
        b[2] = 0xA5;
        b[3] = 0x5A;
        CHECK(roclink_is_node_addr(roclink_node_id_from_bytes(b, sizeof(b))));
    }

    CHECK(!roclink_is_node_addr(ROCLINK_ADDR_ANY_GATEWAY));
    CHECK(!roclink_is_node_addr(ROCLINK_ADDR_GATEWAY_MIN));
    CHECK(!roclink_is_node_addr(ROCLINK_ADDR_BROADCAST));
    CHECK(roclink_is_gateway_addr(0xFF00) && roclink_is_gateway_addr(0xFFFE));
    CHECK(!roclink_is_gateway_addr(ROCLINK_ADDR_BROADCAST));
    CHECK(!roclink_is_gateway_addr(ROCLINK_ADDR_ANY_GATEWAY));
}

/* The build's frame limit, in both directions. */
static void test_frame_size_limit(void) {
    static uint8_t out[ROCLINK_FRAME_MAX_LEN + 16];
    static uint8_t big[ROCLINK_FRAME_MAX_LEN + 16];
    roclink_header_t h = hdr(ROCLINK_TYPE_DATA, 0, 0x1234, 0, 1, 1);
    roclink_header_t d;
    const uint8_t *p = NULL;
    size_t plen = 0;
    int n;

    CHECK_EQ(roclink_frame_max_len(), ROCLINK_FRAME_MAX_LEN); /* library and test agree */
    n = roclink_encode(&h, big, ROCLINK_PAYLOAD_MAX, out, sizeof(out));
    CHECK_EQ(n, ROCLINK_FRAME_MAX_LEN);
    CHECK_EQ(roclink_decode(out, (size_t)n, &d, &p, &plen), ROCLINK_OK);
    CHECK_EQ(plen, ROCLINK_PAYLOAD_MAX);
    CHECK_EQ(roclink_encode(&h, big, ROCLINK_PAYLOAD_MAX + 1, out, sizeof(out)), ROCLINK_E_TOO_LONG);
    /* One byte over the limit on receive: refused, so small builds drop jumbo frames. */
    CHECK_EQ(roclink_decode(out, ROCLINK_FRAME_MAX_LEN + 1, &d, &p, &plen), ROCLINK_E_TOO_LONG);
#if defined(ROCLINK_JUMBO)
    CHECK(ROCLINK_FRAME_MAX_LEN > ROCLINK_STD_FRAME_MAX_LEN);
#else
    CHECK_EQ(ROCLINK_FRAME_MAX_LEN, 255);
#endif
}

int main(void) {
    RUN(test_golden_vectors_encode);
    RUN(test_golden_vectors_decode);
    RUN(test_decode_rejects);
    RUN(test_decode_rejects_malformed_ack);
    RUN(test_decode_secured_layout);
    RUN(test_encode_rejects);
    RUN(test_addresses);
    RUN(test_frame_size_limit);
    return REPORT();
}
