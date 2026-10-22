/*
 * roclink_secure.h - roclink's optional security (PROTOCOL.md section 10).
 *
 *   1. The crypto layer   one AES block function from the platform, and
 *                          roclink's own CCM and CMAC on top of it
 *   2. Security           seal and open, keys and the ratchet, frame counters,
 *                          replay and clone detection, policy
 *
 * A project that sends only unsecured frames can leave this file and
 * roclink_secure.c / roclink_aes.c out. roclink.h never includes it.
 */
#ifndef ROCLINK_SECURE_H
#define ROCLINK_SECURE_H

#include "roclink.h"

/* ======================================================================
 * 1. The crypto layer
 * ====================================================================== */

/*
 * The cryptography under it.
 *
 * roclink needs one primitive from the platform: AES-128 encryption of a
 * single 16-byte block. Everything else is built on it here, in plain C:
 *
 *   AES-128-CCM (NIST SP 800-38C, RFC 3610): seal and open
 *   AES-CMAC (RFC 4493): the key-derivation PRF
 *
 * The block cipher comes from exactly one provider:
 *
 *   ESP32 family    roclink_aes.c          hardware AES through ESP-IDF; automatic on
 *                                          ESP-IDF and Arduino-ESP32 2.x/3.x
 *   PSA Crypto      roclink_aes.c          automatic on Zephyr; -D ROCLINK_CRYPTO_PSA
 *                                          elsewhere (link TF-PSA-Crypto or mbedTLS)
 *   OpenSSL         roclink_aes.c          -D ROCLINK_CRYPTO_OPENSSL, link -lcrypto:
 *                                          Linux gateways and the host tests
 *   roclink-crypto  separate library       every other Arduino board (AVR, RP2040,
 *                                          nRF52, SAMD, STM32, ESP8266), using the
 *                                          rweather Crypto library
 *   your own        -D ROCLINK_CRYPTO_CUSTOM, then define roclink_aes128_encrypt()
 *
 * With no provider, roclink_aes128_encrypt() is simply left undefined. An
 * unsecured build (level none, no roclink_sec_* calls) still links, because
 * the toolchains drop unused code (-ffunction-sections, --gc-sections) or
 * never pull roclink_secure from a library archive. A build that does use
 * security fails at link time with "undefined reference to
 * roclink_aes128_encrypt": install roclink-crypto, or pick a provider.
 * (A weak default would be quieter, but it would beat a provider linked from
 * a library archive, which is how PlatformIO links libraries.)
 *
 * roclink_aes.c holds every built-in provider, each compiled only when
 * selected, so Arduino builds (which compile all of src/) need no setup.
 *
 */

#if (defined(ROCLINK_CRYPTO_PSA) + defined(ROCLINK_CRYPTO_OPENSSL) + \
     defined(ROCLINK_CRYPTO_CUSTOM)) > 1
#error "roclink: select at most one of ROCLINK_CRYPTO_PSA, _OPENSSL, _CUSTOM"
#endif
#if !defined(ROCLINK_CRYPTO_PSA) && !defined(ROCLINK_CRYPTO_OPENSSL) && \
    !defined(ROCLINK_CRYPTO_CUSTOM)
#if defined(ESP_PLATFORM)
#define ROCLINK_CRYPTO_ESP32 1 /* the AES hardware, through ESP-IDF */
#elif defined(__ZEPHYR__)
#define ROCLINK_CRYPTO_PSA 1
#else
#define ROCLINK_CRYPTO_EXTERNAL 1 /* the roclink-crypto library or the application defines it */
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ROCLINK_KEY_LEN 16u   /* AES-128 */
#define ROCLINK_BLOCK_LEN 16u /* the AES block */
#define ROCLINK_NONCE_LEN 13u /* CCM with a 2-byte length field */

/*
 * The provider's one job: out = AES-128-Encrypt(key, in), one block. `out`
 * may equal `in`. Returns 0 on success, non-zero on failure. Providers wipe
 * any key schedule they build. Only the encrypt direction is ever needed.
 */
int roclink_aes128_encrypt(const uint8_t key[ROCLINK_KEY_LEN], const uint8_t in[ROCLINK_BLOCK_LEN],
                           uint8_t out[ROCLINK_BLOCK_LEN]);

/* Results of the CCM functions besides 0 (success). A negative value is a
 * provider failure. */
#define ROCLINK_CCM_BAD_TAG 1

/*
 * AES-128-CCM encryption in place, with a 13-byte nonce and a tag of 4 to 16
 * bytes (roclink uses 4 or 8). On entry buf[0..len) is the plaintext; on
 * return it is the ciphertext, followed by tag_len tag bytes, so buf must have
 * room for len + tag_len. len may be 0 (authenticate associated data only).
 *
 * The associated data is aad followed by aad2 (either may be empty), so a
 * caller can authenticate a header and a payload without copying them together.
 * Together they must be shorter than 0xFF00 bytes, and len at most 0xFFFF.
 */
int roclink_crypto_ccm_seal(const uint8_t key[ROCLINK_KEY_LEN],
                            const uint8_t nonce[ROCLINK_NONCE_LEN], const uint8_t *aad,
                            size_t aad_len, const uint8_t *aad2, size_t aad2_len, uint8_t *buf,
                            size_t len, size_t tag_len);

/*
 * AES-128-CCM verification and decryption in place. On entry buf[0..len) is
 * the ciphertext and buf[len..len+tag_len) the tag. Returns 0 if the tag
 * verifies, and buf[0..len) is then the plaintext. Returns ROCLINK_CCM_BAD_TAG
 * if it doesn't, and buf[0..len) is wiped. The comparison is constant-time.
 */
int roclink_crypto_ccm_open(const uint8_t key[ROCLINK_KEY_LEN],
                            const uint8_t nonce[ROCLINK_NONCE_LEN], const uint8_t *aad,
                            size_t aad_len, const uint8_t *aad2, size_t aad2_len, uint8_t *buf,
                            size_t len, size_t tag_len);

/* AES-CMAC (RFC 4493). Returns 0 on success. */
int roclink_crypto_cmac(const uint8_t key[ROCLINK_KEY_LEN], const uint8_t *msg, size_t len,
                        uint8_t mac[ROCLINK_BLOCK_LEN]);

/*
 * Checks the provider once with a known answer (FIPS-197 / NIST AESAVS:
 * AES-128 of a zero block under a zero key). Returns 0 if it's correct, -1 if
 * it failed or gave the wrong answer. roclink_secure calls it when a key is
 * configured, so a broken custom provider is caught at start-up.
 */
int roclink_crypto_selftest(void);

/* Clears memory in a way the compiler can't drop as a dead store. */
void roclink_crypto_wipe(void *p, size_t n);

/* ======================================================================
 * 2. Security
 * ====================================================================== */

/*
 * the security extension (docs/PROTOCOL.md section 10):
 * AES-128-CCM authentication or encryption, per-node keys with a forward-
 * secure ratchet, frame counters, replay and clone detection, downlink
 * freshness, revocation and receive policy.
 *
 * It wraps the plain protocol instead of living inside it:
 *
 *   send:     roclink_client_build_*() / roclink_server_build_*()  ->  roclink_sec_seal()  ->  radio
 *   receive:  radio  ->  roclink_sec_open()  ->  roclink_client_handle() / roclink_server_handle_opened()
 *
 * so the codec, client and server never depend on cryptography. Depends on
 * roclink_frame and one crypto backend (roclink_crypto.h). Like the rest of
 * roclink: no heap, no clock, no callbacks; all state is caller-owned.
 */

/* ----------------------------------------------------------- configuration */

/* Minimum level accepted from peers (PROTOCOL.md 10.1). 0 is not a policy, so
 * a zero-initialised configuration is rejected rather than silently open. */
typedef enum {
    ROCLINK_POLICY_UNSET = 0,
    ROCLINK_POLICY_REQUIRE_ENC = 1,  /* default */
    ROCLINK_POLICY_REQUIRE_AUTH = 2, /* auth or enc */
    ROCLINK_POLICY_ALLOW_NONE = 3,   /* explicit opt-out: anything, including unsecured */
    ROCLINK_POLICY_DENY = 4          /* per-node only: revoked, refuse everything */
} roclink_sec_policy_t;

/*
 * Gateway: which nodes it accepts without having been told about them first
 * (PROTOCOL.md 10.4). 0 is not a mode, so a zero-initialised gateway
 * configuration is rejected.
 */
typedef enum {
    ROCLINK_ADMIT_UNSET = 0,
    ROCLINK_ADMIT_OPEN = 1,  /* default: with the master, any node whose key derives from it */
    ROCLINK_ADMIT_LISTED = 2 /* only nodes given to roclink_sec_peer_enroll() or _admit() */
} roclink_sec_admission_t;

typedef struct {
    uint8_t net_id;
    uint16_t addr;               /* this device's own node or gateway address */
    roclink_sec_level_t level;   /* for frames this device sends; ROCLINK_SEC_NONE is the opt-out */
    uint8_t tag_len;             /* 8, or 4 where frames are tight (nRF24, slow LoRa) */
    roclink_sec_policy_t policy; /* minimum level accepted from peers (not DENY); see below */
    uint8_t min_tag_len;         /* shortest tag accepted from peers: 8 (or 0, the default) or 4 */
    /* node only */
    bool always_listening; /* mains-powered node class (PROTOCOL.md section 7): accepts
                              gateway frames at any time, bound to its latest uplink */
    /* gateway only */
    roclink_sec_admission_t admission;
    uint16_t max_epoch_skip; /* epochs a node's stored key may be ratcheted for one frame:
                                1..ROCLINK_MAX_EPOCH_SKIP, 0 = ROCLINK_EPOCH_SKIP_DEFAULT */
    bool open_net; /* this net has no secured nodes at all, like the hub's public test
                      net: only then may `policy` be ALLOW_NONE (PROTOCOL.md Annex B) */
} roclink_sec_config_t;

/*
 * Policy ALLOW_NONE (accept unsecured frames from everyone) is allowed only
 * where it can't expose a secured device (PROTOCOL.md 10.1):
 *   - on a gateway, only with open_net set; elsewhere allow `none` per node
 *     with roclink_sec_peer_t.policy instead;
 *   - on a node, only if the node itself is unsecured (level NONE).
 */

/* Defaults: level enc, 8-byte tag, require-enc with 8-byte tags, a sleepy
 * node, open admission, 32-epoch skip. net_id and addr still to set. */
void roclink_sec_config_default(roclink_sec_config_t *cfg);

/* -------------------------------------------------------------------- keys */

/*
 * Key hierarchy (PROTOCOL.md 10.4), all NIST SP 800-108 with AES-CMAC:
 *
 *   master --"roclink node key"--> CK(0)        a node's provisioned chain key
 *   CK(e)  --"roclink chain"-----> CK(e+1)      the ratchet: one way, old keys erased
 *   CK(e)  --"roclink frame"-----> FK(e)        the AES-CCM key for frames in epoch e
 *
 * The epoch of a node frame is fcnt >> ROCLINK_EPOCH_SHIFT; a gateway frame
 * uses the epoch of the node frame that opened the reply.
 */
#define ROCLINK_EPOCH_SHIFT 16u
#define ROCLINK_EPOCH_OF(fcnt) ((uint32_t)(fcnt) >> ROCLINK_EPOCH_SHIFT)
#define ROCLINK_EPOCH_MAX 0xFFFFu
/*
 * How far one frame may move a node's key forward, which bounds the work a
 * forged frame can cause a gateway (one AES-CMAC per epoch):
 *   - from a node's stored chain key: the configured max_epoch_skip, by
 *     default ROCLINK_EPOCH_SKIP_DEFAULT (32 days of daily epochs);
 *   - first contact under open admission, derived from the master: up to
 *     ROCLINK_MAX_EPOCH_SKIP from epoch 0 (with listed admission, max_epoch_skip).
 * Beyond that the frame is refused (ROCLINK_E_EPOCH) and the node is re-enrolled.
 * ROCLINK_MAX_EPOCH_SKIP (about 3 years of daily epochs) is also the hard limit
 * of roclink_sec_chain_advance() and the largest max_epoch_skip allowed.
 */
#define ROCLINK_EPOCH_SKIP_DEFAULT 32u
#define ROCLINK_MAX_EPOCH_SKIP 1024u

/* CK(0) for `node` under master generation `key_gen`. Used by provisioning
 * tools; a gateway holding the master derives it internally. */
roclink_status_t roclink_sec_derive_node_key(const uint8_t master[ROCLINK_KEY_LEN], uint8_t net_id,
                                             uint16_t node, uint8_t key_gen,
                                             uint8_t node_key[ROCLINK_KEY_LEN]);

/* Advances a chain key from epoch `from` to `to` (to >= from, at most
 * ROCLINK_MAX_EPOCH_SKIP apart). For provisioning and enrollment tools. */
roclink_status_t roclink_sec_chain_advance(const uint8_t chain_key[ROCLINK_KEY_LEN], uint8_t net_id,
                                           uint16_t node, uint8_t key_gen, uint32_t from,
                                           uint32_t to, uint8_t out[ROCLINK_KEY_LEN]);

/* ---------------------------------------------------------- frame counter */

/*
 * A sender's frame counter with block reservation (PROTOCOL.md 10.5). The
 * library never touches storage: when a new block is needed it refuses to
 * hand out a counter until the application has persisted the reservation.
 *
 *   boot:      roclink_fcnt_init(&c, value_from_nvs_or_0, 0);
 *   sending:   n = roclink_sec_seal(...);
 *              if (n == ROCLINK_E_PERSIST) {
 *                  uint32_t v = roclink_fcnt_to_persist(&c);
 *                  nvs_write(v);                     // must complete first
 *                  roclink_fcnt_persisted(&c, v);
 *                  n = roclink_sec_seal(...);         // now succeeds
 *              }
 *
 * Keeping the struct in RTC memory across deep sleep avoids the boot-time
 * write; persisting is still what protects against power loss.
 */
#define ROCLINK_FCNT_BLOCK_DEFAULT 256u

typedef struct {
    uint32_t next;  /* next value to use */
    uint32_t limit; /* first value not covered by the persisted reservation */
    uint32_t block;
} roclink_fcnt_t;

/* `stored` is the last value persisted (0 for a new key); block 0 = default. */
void roclink_fcnt_init(roclink_fcnt_t *c, uint32_t stored, uint32_t block);
/* Takes the next counter: ROCLINK_E_PERSIST if a reservation must be stored
 * first, ROCLINK_E_EXHAUSTED once the key is used up. */
roclink_status_t roclink_fcnt_take(roclink_fcnt_t *c, uint32_t *fcnt);
/* The value to store before the next block can be used. */
uint32_t roclink_fcnt_to_persist(const roclink_fcnt_t *c);
/* Call after `value` (from roclink_fcnt_to_persist()) is safely stored. */
void roclink_fcnt_persisted(roclink_fcnt_t *c, uint32_t value);
/* Moves the counter to the start of the next epoch, so the next sealed frame
 * ratchets the key: call it once a day for a daily ratchet. Skipping counter
 * values is always allowed. ROCLINK_E_EXHAUSTED in the last epoch. */
roclink_status_t roclink_fcnt_next_epoch(roclink_fcnt_t *c);

/*
 * Gateway, at every start: skips the counter forward by `n` values
 * (ROCLINK_FCNT_RESTART_SKIP is a sensible default), so a counter restored
 * from an old backup can't re-use values sent after that backup was taken
 * (PROTOCOL.md 10.5). The next take asks for the new position to be persisted.
 * ROCLINK_E_EXHAUSTED if that runs past the end.
 *
 * Not for nodes: a node's counter carries its key epoch in the top 16 bits,
 * so a large skip would ratchet its key. Nodes rely on block reservation, and
 * gateways detect a node whose counter went backwards (ROCLINK_E_CONFLICT,
 * ROCLINK_E_REPLAY).
 */
#define ROCLINK_FCNT_RESTART_SKIP 65536u
roclink_status_t roclink_fcnt_skip(roclink_fcnt_t *c, uint32_t n);

/* ------------------------------------------------------- receive state */

/*
 * What a gateway keeps for one node, and must persist whenever
 * roclink_sec_open() accepts a new frame (info.level != NONE and
 * !info.retransmission), so a restart can't reopen a replay window.
 * It holds the node's current chain key (and the frame key derived from it):
 * keep it in protected storage, store the struct whole, and overwrite the old
 * record so earlier keys are really gone.
 *
 * `policy` is configuration, not state: ROCLINK_POLICY_UNSET (0) = use the
 * network policy; ROCLINK_POLICY_DENY revokes the node; anything else
 * overrides the network policy for this node.
 */
typedef struct {
    uint32_t last_fcnt;
    uint32_t epoch;                      /* epoch of chain_key */
    uint8_t chain_key[ROCLINK_KEY_LEN];  /* the node's current chain key */
    uint8_t frame_key[ROCLINK_KEY_LEN];  /* FK(epoch), so a frame in the current epoch needs no KDF */
    uint8_t last_tag[8];                 /* tag of the last accepted frame (clone detection) */
    uint8_t key_gen;
    uint8_t state;   /* internal */
    uint8_t policy;  /* roclink_sec_policy_t override */
    uint8_t min_tag; /* shortest tag accepted from this node: 0 = the network's, or 4 or 8 */
} roclink_sec_peer_t;

/*
 * The peer state as a fixed-format record, for storage that must survive a
 * rebuild or a move to another machine (the struct's layout depends on the
 * compiler). Little-endian: format version (1) | last_fcnt | epoch |
 * chain_key | frame_key | last_tag | key_gen | state | policy | min_tag.
 * Overwrite the old record in place (SECURITY.md R4).
 */
#define ROCLINK_SEC_PEER_RECORD_LEN 53u
void roclink_sec_peer_save(const roclink_sec_peer_t *peer, uint8_t out[ROCLINK_SEC_PEER_RECORD_LEN]);
/* ROCLINK_E_ARG if the record isn't a version-1 record. */
roclink_status_t roclink_sec_peer_load(roclink_sec_peer_t *peer,
                                       const uint8_t in[ROCLINK_SEC_PEER_RECORD_LEN]);

/* Gives a gateway a node's chain key at `epoch`, from the provisioning tool:
 * how a gateway without the master learns its nodes. Keeps `policy`. */
roclink_status_t roclink_sec_peer_enroll(roclink_sec_peer_t *peer,
                                         const uint8_t chain_key[ROCLINK_KEY_LEN], uint8_t key_gen,
                                         uint32_t epoch);

/* ------------------------------------------------------------- context */

#define ROCLINK_SEC_MAX_KEYS 2u /* gateway: current master generation + one being rotated out */
#define ROCLINK_SEC_REPLY_GATEWAYS 4u /* node: gateways tracked per binding (PROTOCOL.md 10.6) */

/* Treat as opaque. */
typedef struct {
    roclink_sec_config_t cfg;
    bool is_gateway;
    uint8_t key_count;
    uint8_t key_gen[ROCLINK_SEC_MAX_KEYS];
    uint8_t key[ROCLINK_SEC_MAX_KEYS][ROCLINK_KEY_LEN]; /* node: chain key CK(epoch); gateway: masters */
    /* node only */
    uint32_t epoch;
    uint8_t frame_key[ROCLINK_KEY_LEN]; /* FK(epoch) */
    bool key_pending;                   /* a ratcheted key waits to be persisted */
    uint32_t pending_epoch;
    uint8_t pending_key[ROCLINK_KEY_LEN];
    bool have_opener;                   /* reply binding (PROTOCOL.md 10.6) */
    uint32_t opener_fcnt;
    uint8_t reply_count; /* gateways heard from under this binding */
    uint16_t reply_src[ROCLINK_SEC_REPLY_GATEWAYS];
    uint32_t reply_fcnt[ROCLINK_SEC_REPLY_GATEWAYS]; /* the last fcnt accepted from each */
} roclink_sec_t;

/*
 * Node: `chain_key` is this node's chain key at `epoch`: CK(0) as
 * provisioned, or the key last persisted from roclink_sec_key_pending().
 * The key may be NULL only for an explicitly insecure node (level NONE and
 * policy ALLOW_NONE). roclink_sec_init_node() is epoch 0.
 *
 * Init with a key checks the AES provider once (roclink_crypto_selftest()),
 * and fails with ROCLINK_E_CRYPTO if it's wrong.
 */
roclink_status_t roclink_sec_init_node(roclink_sec_t *s, const roclink_sec_config_t *cfg,
                                       const uint8_t chain_key[ROCLINK_KEY_LEN], uint8_t key_gen);
roclink_status_t roclink_sec_init_node_at(roclink_sec_t *s, const roclink_sec_config_t *cfg,
                                          const uint8_t chain_key[ROCLINK_KEY_LEN], uint8_t key_gen,
                                          uint32_t epoch);

/*
 * Node: when roclink_sec_seal() returns ROCLINK_E_PERSIST_KEY, the counter has
 * reached a new epoch and the ratchet has produced the next chain key. Store
 * it (overwriting the old one), then call roclink_sec_key_persisted() and seal
 * again; the old key is erased from memory then.
 *
 *   uint8_t ck[16]; uint32_t e;
 *   if (roclink_sec_key_pending(&sec, ck, &e)) { nvs_write_key(ck, e); roclink_sec_key_persisted(&sec, e); }
 */
bool roclink_sec_key_pending(const roclink_sec_t *s, uint8_t chain_key[ROCLINK_KEY_LEN],
                             uint32_t *epoch);
roclink_status_t roclink_sec_key_persisted(roclink_sec_t *s, uint32_t epoch);

/*
 * Gateway: `master_key` is the network master key; each node's chain key is
 * derived from it and ratcheted forward. Pass NULL to run without the master
 * (better: a stolen gateway then reveals no past traffic), in which case
 * every node must be enrolled with roclink_sec_peer_enroll().
 */
roclink_status_t roclink_sec_init_gateway(roclink_sec_t *s, const roclink_sec_config_t *cfg,
                                          const uint8_t master_key[ROCLINK_KEY_LEN],
                                          uint8_t key_gen);
/*
 * Gateway key rotation: also accept frames under the previous master
 * generation, (key_gen - 1) mod 16. The key given to init stays the current
 * one, the generation nodes are moving to (PROTOCOL.md 10.4): anything else is
 * refused with ROCLINK_E_ARG, because a gateway keeps counters for one
 * generation per node, and only that ordering keeps old frames from being
 * replayed after a node has moved.
 */
roclink_status_t roclink_sec_add_key(roclink_sec_t *s, const uint8_t key[ROCLINK_KEY_LEN],
                                     uint8_t key_gen);
/*
 * Gateway with the master: enrols `node` itself, at `epoch` (0 for a newly
 * provisioned node), under the current master generation. This is how a
 * master-mode gateway with listed admission learns which nodes it accepts;
 * the node list comes from provisioning. Keeps `policy`.
 */
roclink_status_t roclink_sec_peer_admit(const roclink_sec_t *s, roclink_sec_peer_t *peer,
                                        uint16_t node, uint32_t epoch);

/* Zeroes all key material in the context. */
void roclink_sec_wipe(roclink_sec_t *s);

/*
 * Node: the listen window has ended. Until the next frame that opens a reply
 * (LISTEN or ACK_REQ), every gateway frame is refused (ROCLINK_E_UNEXPECTED).
 * Call it when you stop listening. Otherwise a reply stays acceptable until
 * the node's next DATA frame, so a downlink recorded while the node was
 * jammed could be delivered late (PROTOCOL.md 10.6). No effect on a gateway,
 * or on an always-listening node, whose window never closes: every DATA frame
 * it sends moves the binding instead, and its heartbeat bounds a late downlink.
 */
void roclink_sec_close_window(roclink_sec_t *s);

/* ------------------------------------------------------------ seal / open */

/*
 * Turns a plain frame (from a roclink_*_build_* function) into the frame to
 * transmit, at this device's configured level. `out` may equal `in` (sealed in
 * place) but must not otherwise overlap it; it needs room for
 * len + ROCLINK_SEC_OVERHEAD(tag_len). Returns the new length or a negative
 * status. Nothing is written on failure, except after ROCLINK_E_CRYPTO.
 *
 * `ctr` is this device's frame counter. ROCLINK_E_PERSIST: see roclink_fcnt_t.
 * Node, ROCLINK_E_PERSIST_KEY: see roclink_sec_key_pending().
 *
 * Gateway: `opener` is the info from roclink_sec_open() of the node frame that
 * opened this reply, and `peer` that node's state (both required). Replies to
 * an unsecured frame are sent unsecured. Node: pass NULL for both.
 *
 * Gateway, unsolicited downlink to an always-listening node: there's no
 * opener, so pass one built from the hub's binding (PROTOCOL.md 10.6):
 * .level = ROCLINK_SEC_ENC (or NONE for an opted-in unsecured node) and
 * .fcnt = the fcnt of the node's most recent uplink the hub has seen.
 *
 * To retransmit, resend the sealed bytes. A gateway re-offering downlink
 * seals the kept plain frame again (new counter, bound to the new opener).
 */
int roclink_sec_seal(roclink_sec_t *s, roclink_fcnt_t *ctr, const roclink_sec_info_t *opener,
                     const roclink_sec_peer_t *peer, const uint8_t *in, size_t len, uint8_t *out,
                     size_t out_cap);

/*
 * Checks a received frame against policy, verifies it and decrypts it in
 * place, leaving a plain frame for roclink_client_handle() or
 * roclink_server_handle_opened(). Unsecured frames pass through unchanged if
 * the policy allows them. Returns the plain length or a negative status. On
 * failure no state changes and the frame contents must not be used.
 *
 * Worth alerting on at a gateway: ROCLINK_E_CONFLICT (an authentic frame
 * reused a counter: a cloned node or a counter reset), and a burst of
 * ROCLINK_E_REPLAY from one node (a replay attack, or a clone running behind).
 *
 * Gateway: `peer` is the state for the frame's src (required). Node: pass
 * NULL (the node checks freshness through the reply binding).
 *
 * Every check that needs no cryptography runs first, so most bad frames are
 * refused in well under a microsecond: network, addresses, revocation,
 * policy (level and tag length), suite, key generation, admission
 * (ROCLINK_E_NOT_ENROLLED), replay, and how far the key must ratchet
 * (ROCLINK_E_EPOCH).
 */
int roclink_sec_open(roclink_sec_t *s, roclink_sec_peer_t *peer, uint8_t *frame, size_t len,
                     roclink_sec_info_t *info);

/*
 * What roclink_sec_open() would cost, without doing it: the number of key
 * derivations (one AES-CMAC each) it would run before checking the tag, or
 * the negative status it would return without any. Reads only the header, and
 * changes nothing.
 *
 *   0         a frame in the node's current epoch: one AES-CCM check, the normal case
 *   2         the first frame of the next epoch (a daily ratchet)
 *   up to 33  a node's stored key moved the full default 32 epochs
 *   up to 1026  first contact under open admission
 *
 * For gateways to budget expensive opens with their own clock (the library
 * has none): for example, allow one costly frame per node per hour, and a
 * global limit per second, and drop the rest unopened. A node's cost is 0.
 */
int roclink_sec_open_cost(const roclink_sec_t *s, const roclink_sec_peer_t *peer,
                          const uint8_t *frame, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ROCLINK_SECURE_H */
