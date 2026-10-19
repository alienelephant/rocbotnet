/*
 * roclink.h - roclink wire protocol v1: the frame format, and the node
 * (client) and gateway (server) sides. Security is optional and lives in
 * roclink_secure.h.
 *
 *   1. Frame format    constants, header, encode/decode, addresses, results
 *   2. Node (client)   builds uplink, interprets gateway frames
 *   3. Gateway (server) interprets uplink, builds ACKs and downlink
 *
 * Pure C99: no I/O, no allocation, no clock, no platform or SDK headers.
 * The normative description is docs/PROTOCOL.md; keep the two in sync.
 */
#ifndef ROCLINK_H
#define ROCLINK_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROCLINK_VERSION_MAJOR 0
#define ROCLINK_VERSION_MINOR 3
#define ROCLINK_VERSION_PATCH 0

/* ======================================================================
 * 1. Frame format (PROTOCOL.md sections 3-6)
 * ====================================================================== */

/* ---------------------------------------------------------------- sizes */

#define ROCLINK_PROTOCOL_VERSION 1u
#define ROCLINK_HEADER_LEN 9u     /* base header */
#define ROCLINK_RELAY_EXT_LEN 2u  /* relay id, only when ROCLINK_FLAG_RELAYED is set */
#define ROCLINK_HEADER_MAX_LEN (ROCLINK_HEADER_LEN + ROCLINK_RELAY_EXT_LEN)
/*
 * Frame size (PROTOCOL.md section 3).
 *
 * Every implementation accepts frames up to ROCLINK_STD_FRAME_MAX_LEN (255 B,
 * the LoRa limit). Build with -D ROCLINK_JUMBO to allow larger frames on
 * carriers that support them (ESP-NOW v2, UDP); ROCLINK_FRAME_MAX_LEN then
 * defaults to 1470 B and may be set with -D, up to 65535 B. Without
 * ROCLINK_JUMBO the limit is always 255 B, and setting it otherwise is an
 * error. Which carriers may carry jumbo frames is the application's decision.
 *
 * The library never sizes a buffer from this value: every call takes the
 * caller's capacity. A mismatch between builds can only refuse frames, never
 * overflow. roclink_frame_max_len() reports the value the library was built with.
 */
#define ROCLINK_STD_FRAME_MAX_LEN 255u
#if defined(ROCLINK_JUMBO)
#ifndef ROCLINK_FRAME_MAX_LEN
#define ROCLINK_FRAME_MAX_LEN 1470u /* ESP-NOW v2 */
#endif
#if ROCLINK_FRAME_MAX_LEN <= 255
#error "roclink: ROCLINK_JUMBO needs ROCLINK_FRAME_MAX_LEN above 255; for 255-byte frames, leave ROCLINK_JUMBO undefined"
#endif
#if ROCLINK_FRAME_MAX_LEN > 65535
#error "roclink: ROCLINK_FRAME_MAX_LEN can be at most 65535 (the AES-CCM limit with a 13-byte nonce)"
#endif
#if ROCLINK_FRAME_MAX_LEN > INT_MAX
#error "roclink: ROCLINK_FRAME_MAX_LEN must fit in an int on this target (16-bit int: at most 32767)"
#endif
#else
#if defined(ROCLINK_FRAME_MAX_LEN) && ROCLINK_FRAME_MAX_LEN != 255
#error "roclink: ROCLINK_FRAME_MAX_LEN can only change with -D ROCLINK_JUMBO"
#endif
#ifndef ROCLINK_FRAME_MAX_LEN
#define ROCLINK_FRAME_MAX_LEN 255u
#endif
#endif
#define ROCLINK_PAYLOAD_MAX (ROCLINK_FRAME_MAX_LEN - ROCLINK_HEADER_LEN) /* 246 without jumbo */

/* ------------------------------------------------------------ addresses */

#define ROCLINK_ADDR_ANY_GATEWAY 0x0000u /* dst only: whichever gateway hears it */
#define ROCLINK_ADDR_NODE_MIN 0x0001u
#define ROCLINK_ADDR_NODE_MAX 0xFEFFu
#define ROCLINK_ADDR_GATEWAY_MIN 0xFF00u
#define ROCLINK_ADDR_GATEWAY_MAX 0xFFFEu
#define ROCLINK_ADDR_BROADCAST 0xFFFFu /* reserved in v1 */

/* ------------------------------------------------------ message schemas */

/* The header's `schema` byte says which message schema (payload layout and
 * meaning) the payload follows, per device type; LoRaWAN calls the same byte
 * FPort. Schema 0 is roclink's own control messages; applications use 1..255. */
#define ROCLINK_SCHEMA_CONTROL 0u

/* Control operations: the first payload byte of a schema-0 DATA frame
 * (PROTOCOL.md section 7.1). Receivers drop any other operation as malformed. */
#define ROCLINK_CONTROL_PING 0x01u

/* ---------------------------------------------------------- frame types */

#define ROCLINK_TYPE_DATA 0u
#define ROCLINK_TYPE_ACK 1u /* header only; seq echoes the acknowledged frame */

/* ---------------------------------------------------------------- flags */


#define ROCLINK_FLAG_ACK_REQ 0x01u   /* receiver must ACK this frame */
#define ROCLINK_FLAG_LISTEN 0x02u    /* node: I am listening for replies right after this */
#define ROCLINK_FLAG_MORE 0x04u      /* gateway: another frame follows, keep listening */
#define ROCLINK_FLAG_DL_QUEUED 0x08u /* gateway: more downlink waits for you */
#define ROCLINK_FLAG_RELAYED 0x10u   /* reserved (relays): 2-byte relay id follows */
#define ROCLINK_FLAG_HOPS_MASK 0x60u /* reserved (relays): hop count */
#define ROCLINK_FLAG_HOPS_SHIFT 5u
#define ROCLINK_FLAG_SECURED 0x80u  /* security header follows, tag ends the frame */

/* ------------------------------------------- secured layout (no crypto) */

/* A secured frame is: header | sec_ctrl | fcnt (4, LE) | payload | tag.
 * Sealing and opening live in roclink_secure; the codec only knows the layout. */
#define ROCLINK_SEC_HEADER_LEN 5u
#define ROCLINK_SECCTRL_SUITE_MASK 0xC0u /* bits 7..6 */
#define ROCLINK_SECCTRL_SUITE_SHIFT 6u
#define ROCLINK_SECCTRL_SUITE_AES128_CCM 0u
#define ROCLINK_SECCTRL_ENC 0x20u  /* payload encrypted (level enc); clear = level auth */
#define ROCLINK_SECCTRL_TAG8 0x10u /* 8-byte tag; clear = 4-byte tag */
#define ROCLINK_SECCTRL_KEY_GEN_MASK 0x0Fu
#define ROCLINK_SEC_OVERHEAD(tag_len) (ROCLINK_SEC_HEADER_LEN + (tag_len))

/* Security level of a frame (PROTOCOL.md section 10.1). 0 is deliberately not a
 * level, so a zero-initialised configuration is rejected instead of silently
 * meaning "insecure". */
typedef enum {
    ROCLINK_SEC_UNSET = 0,
    ROCLINK_SEC_NONE = 1, /* unsecured */
    ROCLINK_SEC_AUTH = 2, /* authenticated, readable */
    ROCLINK_SEC_ENC = 3   /* authenticated and encrypted */
} roclink_sec_level_t;

/* What roclink_sec_open() learned about a received frame. */
typedef struct {
    roclink_sec_level_t level;
    uint8_t key_gen;
    uint32_t fcnt;       /* valid unless level is ROCLINK_SEC_NONE */
    bool retransmission; /* fcnt equals the last one accepted: ACK again, don't deliver */
} roclink_sec_info_t;

/* --------------------------------------------------------------- status */

typedef enum {
    ROCLINK_OK = 0,
    ROCLINK_E_ARG = -1,        /* NULL pointer or invalid field value */
    ROCLINK_E_NOSPACE = -2,    /* output buffer too small */
    ROCLINK_E_TOO_LONG = -3,   /* frame or payload over this build's size limit */
    ROCLINK_E_SHORT = -4,      /* frame shorter than its header */
    ROCLINK_E_VERSION = -5,    /* unknown protocol version */
    ROCLINK_E_TYPE = -6,       /* reserved frame type */
    ROCLINK_E_NET = -7,        /* frame belongs to another network */
    ROCLINK_E_ADDR = -8,       /* not addressed to us, or invalid source */
    ROCLINK_E_RELAYED = -9,    /* relayed frames are not supported yet */
    ROCLINK_E_UNEXPECTED = -10, /* valid, but not expected now (e.g. a stale ACK) */
    ROCLINK_E_AUTH = -11,       /* authentication failed (forged, corrupted, wrong key) */
    ROCLINK_E_REPLAY = -12,     /* frame counter already used */
    ROCLINK_E_POLICY = -13,     /* security level below the receiver's policy */
    ROCLINK_E_KEY = -14,        /* unknown key generation or unsupported suite */
    ROCLINK_E_PERSIST = -15,    /* persist the frame-counter reservation first */
    ROCLINK_E_EXHAUSTED = -16,  /* frame counter used up: re-key */
    ROCLINK_E_CRYPTO = -17,     /* crypto backend failure */
    ROCLINK_E_REVOKED = -18,    /* sender is revoked (peer policy deny) */
    ROCLINK_E_CONFLICT = -19,   /* authentic frame reuses a counter with different content: clone or counter reset */
    ROCLINK_E_PERSIST_KEY = -20, /* persist the new chain key first (roclink_sec_key_pending) */
    ROCLINK_E_MALFORMED = -21,   /* fields contradict the frame type: an ACK with a payload or a schema */
    ROCLINK_E_PEERS_FULL = -22,  /* gateway: peer table full and this node isn't in it */
    ROCLINK_E_NOT_ENROLLED = -23, /* gateway: node not enrolled or admitted here (listed admission) */
    ROCLINK_E_EPOCH = -24        /* frame's epoch is too far ahead of the node's stored key: re-enrol */
} roclink_status_t;

/* Short name for a status (or any negative return value), for logs. */
const char *roclink_status_str(int status);

/* --------------------------------------------------------------- header */

typedef struct {
    uint8_t type;   /* ROCLINK_TYPE_* */
    uint8_t flags;  /* ROCLINK_FLAG_* */
    uint8_t net_id;
    uint16_t src;
    uint16_t dst;
    uint8_t seq;
    uint8_t schema; /* message schema: 1..255 the application's, 0 roclink control */
    uint16_t relay;   /* on the wire only when ROCLINK_FLAG_RELAYED is set */
    uint8_t sec_ctrl; /* on the wire only when ROCLINK_FLAG_SECURED is set */
    uint32_t fcnt;    /* on the wire only when ROCLINK_FLAG_SECURED is set */
} roclink_header_t;

/* ROCLINK_FRAME_MAX_LEN as the library was compiled. Compare it with the
 * application's own ROCLINK_FRAME_MAX_LEN at startup to catch a build mismatch. */
size_t roclink_frame_max_len(void);

/* Header length implied by `flags` (9, or 11 when relayed), excluding any
 * security header. */
size_t roclink_header_len(uint8_t flags);

/* Tag length (4 or 8) a secured frame with this sec_ctrl carries. */
size_t roclink_sec_tag_len(uint8_t sec_ctrl);

/*
 * Writes header + payload into `out`. Returns the frame length (> 0), or a
 * negative roclink_status_t. Rejects reserved types, ACKs that carry a payload
 * or a non-zero schema, and ROCLINK_FLAG_SECURED: secured frames are produced
 * from plain ones by roclink_sec_seal().
 */
int roclink_encode(const roclink_header_t *h, const uint8_t *payload, size_t payload_len,
                   uint8_t *out, size_t out_cap);

/*
 * Parses one frame. On success `*payload` points into `frame` (no copy).
 * A frame longer than ROCLINK_FRAME_MAX_LEN is refused (ROCLINK_E_TOO_LONG),
 * so a 255-byte build drops jumbo frames. An ACK that carries a payload or a
 * non-zero schema is refused (ROCLINK_E_MALFORMED). Checks version, type and
 * length only; whether the frame is *for us* (network, addresses) is the
 * client's or server's decision.
 * For a secured frame, h->sec_ctrl and h->fcnt are filled in and the payload
 * excludes the security header and the tag (it is still ciphertext).
 */
roclink_status_t roclink_decode(const uint8_t *frame, size_t len, roclink_header_t *h,
                                const uint8_t **payload, size_t *payload_len);

/* ------------------------------------------------------------ addresses */

bool roclink_is_node_addr(uint16_t addr);
bool roclink_is_gateway_addr(uint16_t addr);

/*
 * Derives a stable node address from hardware bytes (MAC, flash UID).
 * FNV-1a, mapped into 0x0001..0xFEFF. Collisions are possible; assign
 * addresses explicitly on large deployments.
 */
uint16_t roclink_node_id_from_bytes(const uint8_t *bytes, size_t len);

/* ------------------------------------------------- receive result (shared) */

/* What a received frame means to the client or server that handled it. */
typedef enum {
    ROCLINK_RX_DROP = 0,  /* not for us or invalid; see roclink_rx_t.status */
    ROCLINK_RX_DATA,      /* new DATA: hand the payload to the application */
    ROCLINK_RX_DUPLICATE, /* retransmission already delivered: do not deliver again */
    ROCLINK_RX_CONTROL,   /* schema 0 (reserved for roclink): not delivered */
    ROCLINK_RX_ACK        /* an ACK; hdr.seq is the acknowledged seq */
} roclink_rx_kind_t;

typedef struct {
    roclink_rx_kind_t kind;
    roclink_status_t status;  /* reason, when kind == ROCLINK_RX_DROP */
    roclink_header_t hdr;
    const uint8_t *payload;   /* points into the caller's frame buffer */
    size_t payload_len;
    bool ack_requested;       /* answer with roclink_*_build_ack(), also for duplicates */
    bool listening;           /* server: sender is listening and we may reply now */
    bool more;                /* client: MORE set, keep listening for another frame */
    roclink_sec_info_t sec;   /* server: as passed to roclink_server_handle_opened() */
} roclink_rx_t;

/* ======================================================================
 * 2. Node (client)
 * ====================================================================== */

/*
 * the node side of the roclink wire protocol.
 *
 * Builds uplink frames and interprets frames from a gateway: ACK matching,
 * downlink duplicate suppression, and building the node's own ACKs.
 *
 * Deliberately has no clock, no radio and no callbacks. The caller moves
 * bytes, decides when to listen and when to retry:
 *
 *   - A retransmission is the *identical* frame bytes sent again. Keep the
 *     buffer from roclink_client_build_data() until the send is finished.
 *   - After sending a frame with ROCLINK_SEND_ACK or ROCLINK_SEND_LISTEN,
 *     listen for replies and pass each one to roclink_client_handle().
 *
 * Independent of the gateway side.
 */

/* Options for roclink_client_build_data(). */
#define ROCLINK_SEND_DEFAULT 0x00u /* fire-and-forget, no reply expected */
#define ROCLINK_SEND_ACK 0x01u     /* ask for an ACK (implies LISTEN) */
#define ROCLINK_SEND_LISTEN 0x02u  /* no ACK, but listen for queued downlink */

typedef struct {
    uint8_t net_id;
    uint16_t node_id;    /* ROCLINK_ADDR_NODE_MIN..MAX */
    uint16_t gateway_id; /* ROCLINK_ADDR_ANY_GATEWAY, or one gateway's address */
} roclink_client_config_t;

/* Treat as opaque; fields are public only so it can live on the stack or in static memory. */
typedef struct {
    roclink_client_config_t cfg;
    uint8_t next_seq;
    uint8_t ack_seq; /* seq of the frame awaiting an ACK */
    bool awaiting_ack;
    bool have_dl;    /* dl_seq holds the last downlink seen */
    uint8_t dl_seq;
    bool dl_queued;  /* last gateway frame said more downlink is waiting */
} roclink_client_t;

/*
 * `first_seq` is the sequence number of the first frame. Use a random value
 * at cold boot, or the value saved from roclink_client_next_seq() before deep
 * sleep, so a gateway does not mistake the first frame for a duplicate.
 */
roclink_status_t roclink_client_init(roclink_client_t *c, const roclink_client_config_t *cfg,
                                     uint8_t first_seq);

/*
 * Builds a new DATA frame for the gateway with message schema `schema` (1..255) and consumes a
 * sequence number. Returns the frame length or a negative roclink_status_t.
 * With ROCLINK_SEND_ACK, the client then waits for the matching ACK (see
 * roclink_client_awaiting_ack()).
 */
int roclink_client_build_data(roclink_client_t *c, uint8_t schema, const uint8_t *data,
                              size_t len, uint8_t options, uint8_t *out, size_t out_cap);

/*
 * Builds a PING (PROTOCOL.md section 7.1): a schema-0 DATA frame whose payload
 * is the one operation byte ROCLINK_CONTROL_PING. `options` as for
 * roclink_client_build_data():
 *   ROCLINK_SEND_DEFAULT  "I'm alive": a heartbeat, no reply expected
 *   ROCLINK_SEND_ACK      a link check: the gateway's ACK proves it's reachable
 *   ROCLINK_SEND_LISTEN   a poll: "send me anything queued"
 * Consumes a sequence number like any DATA frame. Returns the frame length
 * (10 bytes unsecured) or a negative roclink_status_t.
 */
int roclink_client_build_ping(roclink_client_t *c, uint8_t options, uint8_t *out, size_t out_cap);

/*
 * Interprets one received frame. Fills `rx`:
 *   ROCLINK_RX_ACK        the frame we were waiting on was acknowledged
 *   ROCLINK_RX_DATA       new downlink: deliver rx->payload to the application
 *   ROCLINK_RX_DUPLICATE  repeat of the last downlink (same seq, from any
 *                         gateway): do not deliver
 *   ROCLINK_RX_CONTROL    a schema-0 control message (a PING): do not deliver
 *   ROCLINK_RX_DROP       not for us, or a control operation this version doesn't
 *                         know (ROCLINK_E_MALFORMED, not ACKed); rx->status says why
 * With security, pass frames through roclink_sec_open() first; a frame that
 * is still secured is dropped with ROCLINK_E_POLICY.
 * If rx->ack_requested, send roclink_client_build_ack(). If rx->more, keep
 * listening for one more frame.
 */
void roclink_client_handle(roclink_client_t *c, const uint8_t *frame, size_t len,
                           roclink_rx_t *rx);

/* Builds the ACK for a frame that had rx->ack_requested set. */
int roclink_client_build_ack(const roclink_client_t *c, const roclink_rx_t *rx, uint8_t *out,
                             size_t out_cap);

/* Stop waiting for an ACK (the caller gave up retrying). */
void roclink_client_cancel_ack(roclink_client_t *c);

bool roclink_client_awaiting_ack(const roclink_client_t *c);

/* True if the last gateway frame said more downlink is queued for us. */
bool roclink_client_downlink_queued(const roclink_client_t *c);

/* The seq the next frame will use; persist it across deep sleep. */
uint8_t roclink_client_next_seq(const roclink_client_t *c);

/* ======================================================================
 * 3. Gateway (server)
 * ====================================================================== */

/*
 * the gateway side of the roclink wire protocol.
 *
 * Interprets uplink frames (filtering, per-node duplicate suppression) and
 * builds the gateway's ACKs and downlink DATA frames, with one downlink
 * sequence counter per node.
 *
 * No clock, no radio, no callbacks, no allocation:
 *   - time is passed in as `now_ms` (any free-running uint32 millisecond
 *     counter; wrap-around is handled),
 *   - the peer table is memory the caller provides, sized as it likes,
 *   - what to send and when (downlink queues, reply timing) is the caller's.
 *
 * Independent of the node side.
 */

typedef struct {
    uint8_t net_id;
    uint16_t gateway_id;       /* ROCLINK_ADDR_GATEWAY_MIN..MAX */
    bool answer_any;           /* reply to frames sent to ROCLINK_ADDR_ANY_GATEWAY */
    uint32_t dedupe_window_ms; /* same (node, seq) within this window = retransmission */
} roclink_server_config_t;

/* Defaults: net 1, gateway 0xFF00, answer_any, 60 s dedupe window. */
void roclink_server_config_default(roclink_server_config_t *cfg);

/* One tracked node. Caller provides an array of these; treat as opaque. */
typedef struct {
    uint16_t node_id;
    uint8_t state;  /* internal flags */
    uint8_t up_seq; /* last uplink seq */
    uint8_t dl_seq; /* last downlink seq sent to this node */
    uint32_t last_seen_ms;
} roclink_peer_t;

typedef struct {
    roclink_server_config_t cfg;
    roclink_peer_t *peers;
    size_t peer_count;
    uint32_t rng; /* seeds each new peer's downlink seq */
} roclink_server_t;

/*
 * `peers` must stay valid for the server's lifetime. When it is full, the
 * least recently seen node is forgotten (its next retransmission may be
 * delivered twice). `seed` should differ across reboots (any entropy you
 * have) so a node does not mistake the first new downlink for a duplicate.
 */
roclink_status_t roclink_server_init(roclink_server_t *s, const roclink_server_config_t *cfg,
                                     roclink_peer_t *peers, size_t peer_count, uint32_t seed);

/*
 * Interprets one received frame. Fills `rx`:
 *   ROCLINK_RX_DATA       new uplink: deliver rx->payload to the application
 *   ROCLINK_RX_DUPLICATE  retransmission already delivered: do not deliver
 *   ROCLINK_RX_CONTROL    a schema-0 control message (a PING): do not deliver,
 *                         but ACK it if asked
 *   ROCLINK_RX_ACK        node acknowledged downlink rx->hdr.seq
 *   ROCLINK_RX_DROP       not for us; rx->status says why
 * rx->ack_requested: send roclink_server_build_ack() (duplicates too).
 * rx->listening: the node is listening right now and this gateway may reply,
 * so this is the moment to send queued downlink.
 */
void roclink_server_handle(roclink_server_t *s, const uint8_t *frame, size_t len, uint32_t now_ms,
                           roclink_rx_t *rx);

/*
 * The same, for a frame that went through roclink_sec_open() first; `sec` is
 * the info it returned and is copied to rx->sec. A secured frame is a
 * duplicate exactly when its frame counter repeats (sec->retransmission),
 * not by seq. A frame that is still secured is dropped with ROCLINK_E_POLICY.
 * roclink_server_handle() is this with sec == NULL (unsecured frames only).
 */
void roclink_server_handle_opened(roclink_server_t *s, const uint8_t *frame, size_t len,
                                  uint32_t now_ms, const roclink_sec_info_t *sec,
                                  roclink_rx_t *rx);

/*
 * Builds the ACK for an uplink that had rx->ack_requested set. `flags` may
 * be ROCLINK_FLAG_MORE (a downlink follows in this reply) and/or
 * ROCLINK_FLAG_DL_QUEUED (more downlink is waiting for a later reply).
 */
int roclink_server_build_ack(const roclink_server_t *s, const roclink_rx_t *rx, uint8_t flags,
                             uint8_t *out, size_t out_cap);

/*
 * Builds a downlink DATA frame for `node_id` with message schema `schema` (1..255) with the `seq`
 * the hub assigned (PROTOCOL.md section 9 rule 3: the hub keeps one downlink
 * counter per node, so a re-offer through another gateway keeps its seq).
 * `flags` may include ROCLINK_FLAG_ACK_REQ, ROCLINK_FLAG_MORE and
 * ROCLINK_FLAG_DL_QUEUED. Touches no peer state.
 */
int roclink_server_build_data_seq(const roclink_server_t *s, uint16_t node_id, uint8_t schema,
                                  const uint8_t *data, size_t len, uint8_t flags, uint8_t seq,
                                  uint8_t *out, size_t out_cap);

/*
 * For a gateway that runs without a hub, and so numbers downlink itself:
 * builds a downlink DATA frame for `node_id` with message schema `schema` (1..255) with the next
 * per-node downlink seq (returned in *seq_out, to match the node's ACK).
 * The counter lives in the peer table, which is RAM: seed the server
 * differently on each boot (see roclink_server_init()).
 * `flags` may include ROCLINK_FLAG_ACK_REQ, ROCLINK_FLAG_MORE and
 * ROCLINK_FLAG_DL_QUEUED. To re-offer an unacknowledged downlink, resend the
 * identical bytes rather than building a new frame.
 * A node the gateway isn't tracking takes a free peer slot. It never evicts
 * another node: with the table full, it returns ROCLINK_E_PEERS_FULL. (A node
 * that just opened a reply window is always in the table.)
 */
int roclink_server_build_data(roclink_server_t *s, uint16_t node_id, uint8_t schema,
                              const uint8_t *data, size_t len, uint8_t flags, uint32_t now_ms,
                              uint8_t *out, size_t out_cap, uint8_t *seq_out);

#ifdef __cplusplus
}
#endif

#endif /* ROCLINK_H */
