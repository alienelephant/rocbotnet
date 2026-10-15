/* roclink.c - see roclink.h and docs/PROTOCOL.md. */
#include "roclink.h"

#include <string.h>

/* ======================================================================
 * 1. Frame format
 * ====================================================================== */

/* Byte 0: version (bits 7..5), type (bits 4..2), reserved (bits 1..0). */
#define VERSION_SHIFT 5u
#define TYPE_SHIFT 2u
#define TYPE_MASK 0x07u

/* Casting to uint16_t before shifting keeps this defined where int is 16 bits (AVR). */
static uint16_t get_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)get_le16(p) | ((uint32_t)get_le16(p + 2) << 16);
}

static void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

const char *roclink_status_str(int status) {
    switch (status) {
        case ROCLINK_OK: return "ok";
        case ROCLINK_E_ARG: return "invalid argument";
        case ROCLINK_E_NOSPACE: return "buffer too small";
        case ROCLINK_E_TOO_LONG: return "frame too long";
        case ROCLINK_E_SHORT: return "frame too short";
        case ROCLINK_E_VERSION: return "unknown version";
        case ROCLINK_E_TYPE: return "reserved frame type";
        case ROCLINK_E_NET: return "other network";
        case ROCLINK_E_ADDR: return "not addressed to us";
        case ROCLINK_E_RELAYED: return "relayed frame";
        case ROCLINK_E_UNEXPECTED: return "unexpected frame";
        case ROCLINK_E_AUTH: return "authentication failed";
        case ROCLINK_E_REPLAY: return "replayed frame";
        case ROCLINK_E_POLICY: return "below security policy";
        case ROCLINK_E_KEY: return "unknown key or suite";
        case ROCLINK_E_PERSIST: return "persist frame counter first";
        case ROCLINK_E_EXHAUSTED: return "frame counter exhausted";
        case ROCLINK_E_CRYPTO: return "crypto backend error";
        case ROCLINK_E_REVOKED: return "sender revoked";
        case ROCLINK_E_CONFLICT: return "counter reused with different content";
        case ROCLINK_E_PERSIST_KEY: return "persist the new chain key first";
        case ROCLINK_E_MALFORMED: return "malformed frame";
        case ROCLINK_E_PEERS_FULL: return "peer table full";
        case ROCLINK_E_NOT_ENROLLED: return "node not enrolled";
        case ROCLINK_E_EPOCH: return "epoch too far ahead";
        default: return status > 0 ? "ok" : "unknown error";
    }
}

size_t roclink_frame_max_len(void) {
    return ROCLINK_FRAME_MAX_LEN;
}

size_t roclink_header_len(uint8_t flags) {
    return (flags & ROCLINK_FLAG_RELAYED) ? ROCLINK_HEADER_MAX_LEN : ROCLINK_HEADER_LEN;
}

size_t roclink_sec_tag_len(uint8_t sec_ctrl) {
    return (sec_ctrl & ROCLINK_SECCTRL_TAG8) ? 8u : 4u;
}

int roclink_encode(const roclink_header_t *h, const uint8_t *payload, size_t payload_len,
                   uint8_t *out, size_t out_cap) {
    size_t hl;
    if (!h || !out || (payload_len && !payload)) return ROCLINK_E_ARG;
    if (h->type != ROCLINK_TYPE_DATA && h->type != ROCLINK_TYPE_ACK) return ROCLINK_E_ARG;
    if (h->flags & ROCLINK_FLAG_SECURED) return ROCLINK_E_ARG; /* use roclink_sec_seal() */
    if (h->type == ROCLINK_TYPE_ACK && (payload_len || h->schema != ROCLINK_SCHEMA_CONTROL)) {
        return ROCLINK_E_ARG;
    }
    if (payload_len > ROCLINK_PAYLOAD_MAX) return ROCLINK_E_TOO_LONG;
    hl = roclink_header_len(h->flags);
    if (hl + payload_len > ROCLINK_FRAME_MAX_LEN) return ROCLINK_E_TOO_LONG;
    if (hl + payload_len > out_cap) return ROCLINK_E_NOSPACE;

    out[0] = (uint8_t)((ROCLINK_PROTOCOL_VERSION << VERSION_SHIFT) |
                       ((h->type & TYPE_MASK) << TYPE_SHIFT));
    out[1] = h->flags;
    out[2] = h->net_id;
    put_le16(out + 3, h->src);
    put_le16(out + 5, h->dst);
    out[7] = h->seq;
    out[8] = h->schema;
    if (h->flags & ROCLINK_FLAG_RELAYED) put_le16(out + 9, h->relay);
    if (payload_len) memcpy(out + hl, payload, payload_len);
    return (int)(hl + payload_len);
}

roclink_status_t roclink_decode(const uint8_t *frame, size_t len, roclink_header_t *h,
                                const uint8_t **payload, size_t *payload_len) {
    size_t hl;
    if (!frame || !h || !payload || !payload_len) return ROCLINK_E_ARG;
    if (len < ROCLINK_HEADER_LEN) return ROCLINK_E_SHORT;
    if (len > ROCLINK_FRAME_MAX_LEN) return ROCLINK_E_TOO_LONG;
    if ((frame[0] >> VERSION_SHIFT) != ROCLINK_PROTOCOL_VERSION) return ROCLINK_E_VERSION;
    h->type = (uint8_t)((frame[0] >> TYPE_SHIFT) & TYPE_MASK);
    if (h->type != ROCLINK_TYPE_DATA && h->type != ROCLINK_TYPE_ACK) return ROCLINK_E_TYPE;
    h->flags = frame[1];
    h->net_id = frame[2];
    h->src = get_le16(frame + 3);
    h->dst = get_le16(frame + 5);
    h->seq = frame[7];
    h->schema = frame[8];
    hl = roclink_header_len(h->flags);
    if (len < hl) return ROCLINK_E_SHORT;
    h->relay = (h->flags & ROCLINK_FLAG_RELAYED) ? get_le16(frame + 9) : 0u;
    h->sec_ctrl = 0;
    h->fcnt = 0;
    if (h->flags & ROCLINK_FLAG_SECURED) {
        size_t tag;
        if (len < hl + ROCLINK_SEC_HEADER_LEN) return ROCLINK_E_SHORT;
        h->sec_ctrl = frame[hl];
        h->fcnt = get_le32(frame + hl + 1);
        tag = roclink_sec_tag_len(h->sec_ctrl);
        hl += ROCLINK_SEC_HEADER_LEN;
        if (len < hl + tag) return ROCLINK_E_SHORT;
        len -= tag;
    }
    /* An ACK is a header only (PROTOCOL.md section 6). */
    if (h->type == ROCLINK_TYPE_ACK && (len > hl || h->schema != ROCLINK_SCHEMA_CONTROL)) {
        return ROCLINK_E_MALFORMED;
    }
    *payload = frame + hl;
    *payload_len = len - hl;
    return ROCLINK_OK;
}

bool roclink_is_node_addr(uint16_t addr) {
    return addr >= ROCLINK_ADDR_NODE_MIN && addr <= ROCLINK_ADDR_NODE_MAX;
}

bool roclink_is_gateway_addr(uint16_t addr) {
    return addr >= ROCLINK_ADDR_GATEWAY_MIN && addr <= ROCLINK_ADDR_GATEWAY_MAX;
}

uint16_t roclink_node_id_from_bytes(const uint8_t *bytes, size_t len) {
    uint32_t h = 2166136261UL; /* FNV-1a 32 */
    size_t i;
    for (i = 0; bytes && i < len; ++i) {
        h ^= bytes[i];
        h *= 16777619UL;
    }
    return (uint16_t)(ROCLINK_ADDR_NODE_MIN +
                      (h % (uint32_t)(ROCLINK_ADDR_NODE_MAX - ROCLINK_ADDR_NODE_MIN + 1u)));
}

/* A received frame that isn't for us, or is invalid: rx->status says why. */
static void drop(roclink_rx_t *rx, roclink_status_t why) {
    rx->kind = ROCLINK_RX_DROP;
    rx->status = why;
}

/* ======================================================================
 * 2. Node (client)
 * ====================================================================== */

roclink_status_t roclink_client_init(roclink_client_t *c, const roclink_client_config_t *cfg,
                                     uint8_t first_seq) {
    if (!c || !cfg || !roclink_is_node_addr(cfg->node_id)) return ROCLINK_E_ARG;
    if (cfg->gateway_id != ROCLINK_ADDR_ANY_GATEWAY && !roclink_is_gateway_addr(cfg->gateway_id)) {
        return ROCLINK_E_ARG;
    }
    memset(c, 0, sizeof(*c));
    c->cfg = *cfg;
    c->next_seq = first_seq;
    return ROCLINK_OK;
}

int roclink_client_build_data(roclink_client_t *c, uint8_t schema, const uint8_t *data,
                              size_t len, uint8_t options, uint8_t *out, size_t out_cap) {
    roclink_header_t h;
    int n;
    if (!c || schema == ROCLINK_SCHEMA_CONTROL) return ROCLINK_E_ARG;
    if (options & ~(uint8_t)(ROCLINK_SEND_ACK | ROCLINK_SEND_LISTEN)) return ROCLINK_E_ARG;

    h.type = ROCLINK_TYPE_DATA;
    h.flags = 0;
    if (options & ROCLINK_SEND_ACK) h.flags |= ROCLINK_FLAG_ACK_REQ | ROCLINK_FLAG_LISTEN;
    if (options & ROCLINK_SEND_LISTEN) h.flags |= ROCLINK_FLAG_LISTEN;
    h.net_id = c->cfg.net_id;
    h.src = c->cfg.node_id;
    h.dst = c->cfg.gateway_id;
    h.seq = c->next_seq;
    h.schema = schema;
    h.relay = 0;

    n = roclink_encode(&h, data, len, out, out_cap);
    if (n < 0) return n;
    c->next_seq = (uint8_t)(c->next_seq + 1u);
    c->awaiting_ack = (h.flags & ROCLINK_FLAG_ACK_REQ) != 0;
    c->ack_seq = h.seq;
    return n;
}


void roclink_client_handle(roclink_client_t *c, const uint8_t *frame, size_t len,
                           roclink_rx_t *rx) {
    roclink_header_t *h;
    roclink_status_t st;
    if (!rx) return;
    memset(rx, 0, sizeof(*rx));
    if (!c) {
        drop(rx, ROCLINK_E_ARG);
        return;
    }
    h = &rx->hdr;
    st = roclink_decode(frame, len, h, &rx->payload, &rx->payload_len);
    if (st != ROCLINK_OK) {
        drop(rx, st);
        return;
    }
    if (h->net_id != c->cfg.net_id) {
        drop(rx, ROCLINK_E_NET);
        return;
    }
    /* Replies come from a gateway, to us. Broadcast downlink is reserved in v1. */
    if (!roclink_is_gateway_addr(h->src) || h->dst != c->cfg.node_id ||
        (c->cfg.gateway_id != ROCLINK_ADDR_ANY_GATEWAY && h->src != c->cfg.gateway_id)) {
        drop(rx, ROCLINK_E_ADDR);
        return;
    }
    if (h->flags & ROCLINK_FLAG_RELAYED) {
        drop(rx, ROCLINK_E_RELAYED);
        return;
    }
    if (h->flags & ROCLINK_FLAG_SECURED) { /* must go through roclink_sec_open() first */
        drop(rx, ROCLINK_E_POLICY);
        return;
    }

    if (h->type == ROCLINK_TYPE_ACK) {
        if (!c->awaiting_ack || h->seq != c->ack_seq) {
            drop(rx, ROCLINK_E_UNEXPECTED); /* a dropped frame changes nothing (rule 5) */
            return;
        }
        c->awaiting_ack = false;
        rx->kind = ROCLINK_RX_ACK;
    } else {
        /* DATA. The hub numbers each node's downlink (PROTOCOL.md 9 rule 3), so
         * a re-offer keeps its seq even through another gateway, and a new
         * downlink always has a new one: seq alone decides. */
        rx->ack_requested = (h->flags & ROCLINK_FLAG_ACK_REQ) != 0; /* ACK duplicates too */
        if (c->have_dl && c->dl_seq == h->seq) {
            rx->kind = ROCLINK_RX_DUPLICATE;
        } else {
            rx->kind = h->schema == ROCLINK_SCHEMA_CONTROL ? ROCLINK_RX_CONTROL : ROCLINK_RX_DATA;
        }
        c->have_dl = true;
        c->dl_seq = h->seq;
    }
    /* Only an accepted frame updates these. */
    c->dl_queued = (h->flags & ROCLINK_FLAG_DL_QUEUED) != 0;
    rx->more = (h->flags & ROCLINK_FLAG_MORE) != 0;
}

int roclink_client_build_ack(const roclink_client_t *c, const roclink_rx_t *rx, uint8_t *out,
                             size_t out_cap) {
    roclink_header_t h;
    if (!c || !rx || !rx->ack_requested) return ROCLINK_E_ARG;
    h.type = ROCLINK_TYPE_ACK;
    h.flags = 0;
    h.net_id = c->cfg.net_id;
    h.src = c->cfg.node_id;
    h.dst = rx->hdr.src;
    h.seq = rx->hdr.seq;
    h.schema = ROCLINK_SCHEMA_CONTROL;
    h.relay = 0;
    return roclink_encode(&h, NULL, 0, out, out_cap);
}

void roclink_client_cancel_ack(roclink_client_t *c) {
    if (c) c->awaiting_ack = false;
}

bool roclink_client_awaiting_ack(const roclink_client_t *c) {
    return c && c->awaiting_ack;
}

bool roclink_client_downlink_queued(const roclink_client_t *c) {
    return c && c->dl_queued;
}

uint8_t roclink_client_next_seq(const roclink_client_t *c) {
    return c ? c->next_seq : 0u;
}

/* ======================================================================
 * 3. Gateway (server)
 * ====================================================================== */

#define PEER_USED 0x01u
#define PEER_HAVE_UP 0x02u /* up_seq is valid */

void roclink_server_config_default(roclink_server_config_t *cfg) {
    if (!cfg) return;
    cfg->net_id = 1;
    cfg->gateway_id = ROCLINK_ADDR_GATEWAY_MIN;
    cfg->answer_any = true;
    cfg->dedupe_window_ms = 60000u;
}

static uint8_t next_random(roclink_server_t *s) {
    uint32_t x = s->rng; /* xorshift32 */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x;
    return (uint8_t)(x >> 24);
}

roclink_status_t roclink_server_init(roclink_server_t *s, const roclink_server_config_t *cfg,
                                     roclink_peer_t *peers, size_t peer_count, uint32_t seed) {
    if (!s || !cfg || !peers || peer_count == 0 || !roclink_is_gateway_addr(cfg->gateway_id)) {
        return ROCLINK_E_ARG;
    }
    memset(s, 0, sizeof(*s));
    memset(peers, 0, peer_count * sizeof(*peers));
    s->cfg = *cfg;
    s->peers = peers;
    s->peer_count = peer_count;
    s->rng = seed ? seed : 0x9E3779B9u; /* xorshift must not start at 0 */
    return ROCLINK_OK;
}

/* Finds `node`, or takes a free slot, or (if `may_evict`) evicts the least
 * recently seen peer. NULL if the table is full and eviction isn't allowed. */
static roclink_peer_t *touch_peer(roclink_server_t *s, uint16_t node, uint32_t now_ms,
                                  bool may_evict) {
    roclink_peer_t *free_slot = NULL;
    roclink_peer_t *oldest = NULL;
    roclink_peer_t *p;
    size_t i;
    for (i = 0; i < s->peer_count; ++i) {
        p = &s->peers[i];
        if (!(p->state & PEER_USED)) {
            if (!free_slot) free_slot = p;
        } else if (p->node_id == node) {
            return p;
        } else if (!oldest || (uint32_t)(now_ms - p->last_seen_ms) >
                                  (uint32_t)(now_ms - oldest->last_seen_ms)) {
            oldest = p;
        }
    }
    p = free_slot ? free_slot : (may_evict ? oldest : NULL);
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->state = PEER_USED;
    p->node_id = node;
    p->dl_seq = next_random(s);
    p->last_seen_ms = now_ms;
    return p;
}


void roclink_server_handle(roclink_server_t *s, const uint8_t *frame, size_t len, uint32_t now_ms,
                           roclink_rx_t *rx) {
    roclink_server_handle_opened(s, frame, len, now_ms, NULL, rx);
}

void roclink_server_handle_opened(roclink_server_t *s, const uint8_t *frame, size_t len,
                                  uint32_t now_ms, const roclink_sec_info_t *sec,
                                  roclink_rx_t *rx) {
    roclink_header_t *h;
    roclink_peer_t *p;
    roclink_status_t st;
    bool may_reply;
    bool dup;
    if (!rx) return;
    memset(rx, 0, sizeof(*rx));
    if (!s) {
        drop(rx, ROCLINK_E_ARG);
        return;
    }
    if (sec) rx->sec = *sec;
    else rx->sec.level = ROCLINK_SEC_NONE;
    h = &rx->hdr;
    st = roclink_decode(frame, len, h, &rx->payload, &rx->payload_len);
    if (st != ROCLINK_OK) {
        drop(rx, st);
        return;
    }
    if (h->net_id != s->cfg.net_id) {
        drop(rx, ROCLINK_E_NET);
        return;
    }
    if (!roclink_is_node_addr(h->src) ||
        (h->dst != s->cfg.gateway_id && h->dst != ROCLINK_ADDR_ANY_GATEWAY)) {
        drop(rx, ROCLINK_E_ADDR);
        return;
    }
    if (h->flags & ROCLINK_FLAG_RELAYED) {
        drop(rx, ROCLINK_E_RELAYED);
        return;
    }
    if (h->flags & ROCLINK_FLAG_SECURED) { /* must go through roclink_sec_open() first */
        drop(rx, ROCLINK_E_POLICY);
        return;
    }

    if (h->type == ROCLINK_TYPE_ACK) {
        rx->kind = ROCLINK_RX_ACK; /* the caller matches hdr.seq to its downlink */
        return;
    }

    /* A frame sent to "any gateway" is still delivered by a gateway that does
     * not answer it: the hub may hear it through several gateways. */
    may_reply = h->dst == s->cfg.gateway_id || s->cfg.answer_any;
    rx->ack_requested = may_reply && (h->flags & ROCLINK_FLAG_ACK_REQ) != 0;
    rx->listening = may_reply && (h->flags & (ROCLINK_FLAG_ACK_REQ | ROCLINK_FLAG_LISTEN)) != 0;

    p = touch_peer(s, h->src, now_ms, true);
    if (rx->sec.level == ROCLINK_SEC_AUTH || rx->sec.level == ROCLINK_SEC_ENC) {
        dup = rx->sec.retransmission; /* the frame counter decides (PROTOCOL.md 9.2) */
    } else {
        dup = (p->state & PEER_HAVE_UP) && p->up_seq == h->seq &&
              (uint32_t)(now_ms - p->last_seen_ms) < s->cfg.dedupe_window_ms;
    }
    p->state |= PEER_HAVE_UP;
    p->up_seq = h->seq;
    p->last_seen_ms = now_ms;

    if (dup) {
        rx->kind = ROCLINK_RX_DUPLICATE;
    } else {
        rx->kind = h->schema == ROCLINK_SCHEMA_CONTROL ? ROCLINK_RX_CONTROL : ROCLINK_RX_DATA;
    }
}

int roclink_server_build_ack(const roclink_server_t *s, const roclink_rx_t *rx, uint8_t flags,
                             uint8_t *out, size_t out_cap) {
    roclink_header_t h;
    if (!s || !rx || !rx->ack_requested) return ROCLINK_E_ARG;
    if (flags & ~(uint8_t)(ROCLINK_FLAG_MORE | ROCLINK_FLAG_DL_QUEUED)) return ROCLINK_E_ARG;
    h.type = ROCLINK_TYPE_ACK;
    h.flags = flags;
    h.net_id = s->cfg.net_id;
    h.src = s->cfg.gateway_id;
    h.dst = rx->hdr.src;
    h.seq = rx->hdr.seq;
    h.schema = ROCLINK_SCHEMA_CONTROL;
    h.relay = 0;
    return roclink_encode(&h, NULL, 0, out, out_cap);
}

int roclink_server_build_data_seq(const roclink_server_t *s, uint16_t node_id, uint8_t schema,
                                  const uint8_t *data, size_t len, uint8_t flags, uint8_t seq,
                                  uint8_t *out, size_t out_cap) {
    const uint8_t allowed = ROCLINK_FLAG_ACK_REQ | ROCLINK_FLAG_MORE | ROCLINK_FLAG_DL_QUEUED;
    roclink_header_t h;
    if (!s || !roclink_is_node_addr(node_id) || schema == ROCLINK_SCHEMA_CONTROL) return ROCLINK_E_ARG;
    if (flags & (uint8_t)~allowed) return ROCLINK_E_ARG;
    h.type = ROCLINK_TYPE_DATA;
    h.flags = flags;
    h.net_id = s->cfg.net_id;
    h.src = s->cfg.gateway_id;
    h.dst = node_id;
    h.seq = seq;
    h.schema = schema;
    h.relay = 0;
    return roclink_encode(&h, data, len, out, out_cap);
}

int roclink_server_build_data(roclink_server_t *s, uint16_t node_id, uint8_t schema,
                              const uint8_t *data, size_t len, uint8_t flags, uint32_t now_ms,
                              uint8_t *out, size_t out_cap, uint8_t *seq_out) {
    roclink_peer_t *p;
    uint8_t seq;
    int n;
    if (!s || !roclink_is_node_addr(node_id) || schema == ROCLINK_SCHEMA_CONTROL) return ROCLINK_E_ARG;

    /* A downlink never evicts a node: only frames from nodes do. */
    p = touch_peer(s, node_id, now_ms, false);
    if (!p) return ROCLINK_E_PEERS_FULL;
    seq = (uint8_t)(p->dl_seq + 1u);
    n = roclink_server_build_data_seq(s, node_id, schema, data, len, flags, seq, out, out_cap);
    if (n < 0) return n;
    p->dl_seq = seq; /* only consume a seq once a frame was produced */
    if (seq_out) *seq_out = seq;
    return n;
}
