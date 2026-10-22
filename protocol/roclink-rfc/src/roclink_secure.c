/* roclink_secure.c - see roclink_secure.h and docs/PROTOCOL.md section 10. */
#include "roclink_secure.h"

#include <string.h>

/* ======================================================================
 * 1a. AES-128-CCM
 * ====================================================================== */

/*
 * AES-128-CCM (NIST SP 800-38C, RFC 3610) over the one
 * primitive a provider supplies, roclink_aes128_encrypt().
 *
 * CCM is two passes with the same key:
 *   1. CBC-MAC over B0 | the associated data | the plaintext, which gives the tag;
 *   2. CTR mode, which encrypts the plaintext with blocks A1, A2, ... and
 *      hides the tag with A0.
 * With a 13-byte nonce the length field L is 2 bytes, so a message is at most
 * 65,535 bytes, and the counter in each A block is 2 bytes too.
 */

#define L_LEN 2u          /* bytes in the length field: 15 - nonce length */
#define AAD_LIMIT 0xFF00u /* longer AAD needs a 6-byte length encoding: never used here */

static void xor_block(uint8_t *x, const uint8_t *b, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) x[i] = (uint8_t)(x[i] ^ b[i]);
}

/* CBC-MAC state: x is the running block, `fill` how many bytes of the next
 * block have been XORed into it. Feeding the AAD and the message through one
 * stream gives exactly CCM's zero padding at each boundary we flush. */
typedef struct {
    const uint8_t *key;
    uint8_t x[ROCLINK_BLOCK_LEN];
    size_t fill;
} mac_t;

static int mac_feed(mac_t *m, const uint8_t *p, size_t n) {
    while (n) {
        size_t take = ROCLINK_BLOCK_LEN - m->fill;
        if (take > n) take = n;
        xor_block(m->x + m->fill, p, take);
        m->fill += take;
        p += take;
        n -= take;
        if (m->fill == ROCLINK_BLOCK_LEN) {
            if (roclink_aes128_encrypt(m->key, m->x, m->x) != 0) return -1;
            m->fill = 0;
        }
    }
    return 0;
}

/* Zero-pads to the end of the current block (XORing zeros changes nothing). */
static int mac_flush(mac_t *m) {
    if (m->fill == 0) return 0;
    m->fill = 0;
    return roclink_aes128_encrypt(m->key, m->x, m->x);
}

/* The tag before it's hidden: CBC-MAC of B0, the encoded AAD, and the plaintext. */
static int cbc_mac(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                   const uint8_t *aad2, size_t aad2_len, const uint8_t *msg, size_t len,
                   size_t tag_len, uint8_t x[ROCLINK_BLOCK_LEN]) {
    const size_t total_aad = aad_len + aad2_len;
    mac_t m;
    uint8_t b0[ROCLINK_BLOCK_LEN];
    int rc;

    /* B0: flags | nonce | message length. Flags: Adata, (M-2)/2, L-1. */
    b0[0] = (uint8_t)((total_aad ? 0x40u : 0u) | (((tag_len - 2u) / 2u) << 3) | (L_LEN - 1u));
    memcpy(b0 + 1, nonce, ROCLINK_NONCE_LEN);
    b0[14] = (uint8_t)((len >> 8) & 0xFFu);
    b0[15] = (uint8_t)(len & 0xFFu);
    memset(&m, 0, sizeof(m));
    m.key = key;
    rc = mac_feed(&m, b0, sizeof(b0));

    /* The AAD, prefixed by its 2-byte length, padded to a block. */
    if (rc == 0 && total_aad) {
        uint8_t enc_len[2];
        enc_len[0] = (uint8_t)((total_aad >> 8) & 0xFFu);
        enc_len[1] = (uint8_t)(total_aad & 0xFFu);
        rc = mac_feed(&m, enc_len, 2);
        if (rc == 0) rc = mac_feed(&m, aad, aad_len);
        if (rc == 0) rc = mac_feed(&m, aad2, aad2_len);
        if (rc == 0) rc = mac_flush(&m);
    }
    /* The plaintext, padded to a block. */
    if (rc == 0) rc = mac_feed(&m, msg, len);
    if (rc == 0) rc = mac_flush(&m);
    memcpy(x, m.x, ROCLINK_BLOCK_LEN);
    roclink_crypto_wipe(&m, sizeof(m));
    return rc;
}

/* Counter block A_i: flags (L-1) | nonce | i. */
static void ctr_block(uint8_t a[ROCLINK_BLOCK_LEN], const uint8_t *nonce, size_t i) {
    a[0] = (uint8_t)(L_LEN - 1u);
    memcpy(a + 1, nonce, ROCLINK_NONCE_LEN);
    a[14] = (uint8_t)((i >> 8) & 0xFFu);
    a[15] = (uint8_t)(i & 0xFFu);
}

/* XORs buf with the key stream E(A1) | E(A2) | ...: encrypts or decrypts. */
static int ctr_crypt(const uint8_t *key, const uint8_t *nonce, uint8_t *buf, size_t len) {
    uint8_t a[ROCLINK_BLOCK_LEN], s[ROCLINK_BLOCK_LEN];
    size_t i = 1u, off = 0;
    int rc = 0;
    while (off < len && rc == 0) {
        const size_t n = len - off < ROCLINK_BLOCK_LEN ? len - off : ROCLINK_BLOCK_LEN;
        ctr_block(a, nonce, i++);
        rc = roclink_aes128_encrypt(key, a, s);
        if (rc == 0) xor_block(buf + off, s, n);
        off += n;
    }
    roclink_crypto_wipe(s, sizeof(s));
    return rc;
}

/* The tag as sent: the CBC-MAC XOR E(A0), truncated to tag_len. */
static int hide_tag(const uint8_t *key, const uint8_t *nonce, uint8_t x[ROCLINK_BLOCK_LEN]) {
    uint8_t a[ROCLINK_BLOCK_LEN], s0[ROCLINK_BLOCK_LEN];
    int rc;
    ctr_block(a, nonce, 0);
    rc = roclink_aes128_encrypt(key, a, s0);
    if (rc == 0) xor_block(x, s0, ROCLINK_BLOCK_LEN);
    roclink_crypto_wipe(s0, sizeof(s0));
    return rc;
}

static int bad_args(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                    const uint8_t *aad2, size_t aad2_len, const uint8_t *buf, size_t len,
                    size_t tag_len) {
    return !key || !nonce || (aad_len && !aad) || (aad2_len && !aad2) || !buf ||
           tag_len < 4u || tag_len > 16u || (tag_len & 1u) || len > 0xFFFFu ||
           aad_len >= AAD_LIMIT || aad2_len >= AAD_LIMIT - aad_len;
}

int roclink_crypto_ccm_seal(const uint8_t key[ROCLINK_KEY_LEN],
                            const uint8_t nonce[ROCLINK_NONCE_LEN], const uint8_t *aad,
                            size_t aad_len, const uint8_t *aad2, size_t aad2_len, uint8_t *buf,
                            size_t len, size_t tag_len) {
    uint8_t x[ROCLINK_BLOCK_LEN];
    int rc;
    if (bad_args(key, nonce, aad, aad_len, aad2, aad2_len, buf, len, tag_len)) return -1;
    /* MAC the plaintext first, then encrypt it in place. */
    rc = cbc_mac(key, nonce, aad, aad_len, aad2, aad2_len, buf, len, tag_len, x);
    if (rc == 0) rc = ctr_crypt(key, nonce, buf, len);
    if (rc == 0) rc = hide_tag(key, nonce, x);
    if (rc == 0) memcpy(buf + len, x, tag_len);
    roclink_crypto_wipe(x, sizeof(x));
    return rc == 0 ? 0 : -1;
}

int roclink_crypto_ccm_open(const uint8_t key[ROCLINK_KEY_LEN],
                            const uint8_t nonce[ROCLINK_NONCE_LEN], const uint8_t *aad,
                            size_t aad_len, const uint8_t *aad2, size_t aad2_len, uint8_t *buf,
                            size_t len, size_t tag_len) {
    uint8_t x[ROCLINK_BLOCK_LEN];
    uint8_t diff = 0;
    size_t i;
    int rc;
    if (bad_args(key, nonce, aad, aad_len, aad2, aad2_len, buf, len, tag_len)) return -1;
    /* Decrypt in place, MAC the plaintext, and compare the tags. */
    rc = ctr_crypt(key, nonce, buf, len);
    if (rc == 0) rc = cbc_mac(key, nonce, aad, aad_len, aad2, aad2_len, buf, len, tag_len, x);
    if (rc == 0) rc = hide_tag(key, nonce, x);
    if (rc == 0) {
        for (i = 0; i < tag_len; ++i) diff = (uint8_t)(diff | (x[i] ^ buf[len + i])); /* constant time */
    }
    roclink_crypto_wipe(x, sizeof(x));
    if (rc != 0 || diff != 0) {
        roclink_crypto_wipe(buf, len); /* never hand out unauthenticated plaintext */
        return rc != 0 ? -1 : ROCLINK_CCM_BAD_TAG;
    }
    return 0;
}

/* ------------------------------------------------------- shared helpers */

void roclink_crypto_wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

int roclink_crypto_selftest(void) {
    /* AES-128(key = 0, block = 0), FIPS-197 / NIST AESAVS GFSbox. */
    static const uint8_t expect[ROCLINK_BLOCK_LEN] = {0x66, 0xE9, 0x4B, 0xD4, 0xEF, 0x8A,
                                                      0x2C, 0x3B, 0x88, 0x4C, 0xFA, 0x59,
                                                      0xCA, 0x34, 0x2B, 0x2E};
    const uint8_t zero[ROCLINK_BLOCK_LEN] = {0};
    uint8_t out[ROCLINK_BLOCK_LEN];
    const int rc = roclink_aes128_encrypt(zero, zero, out);
    return rc == 0 && memcmp(out, expect, sizeof(out)) == 0 ? 0 : -1;
}

/* ======================================================================
 * 1b. AES-CMAC, and the key-derivation function built on it
 * ====================================================================== */

/*
 * AES-CMAC (RFC 4493) over roclink_aes128_encrypt(). roclink
 * uses it only as the PRF of its key derivation (below).
 *
 * CMAC is CBC-MAC with a twist for the last block: it is XORed with K1 when
 * it is a full block, or padded with 10...0 and XORed with K2 when it isn't
 * (an empty message counts as one padded block). K1 and K2 come from
 * L = AES(K, 0) by doubling in GF(2^128).
 */

/* Doubling in GF(2^128): shift left one bit, and reduce by x^128 + x^7 + x^2 + x + 1. */
static void dbl(uint8_t b[ROCLINK_BLOCK_LEN]) {
    const uint8_t carry = (uint8_t)(b[0] >> 7);
    size_t i;
    for (i = 0; i + 1u < ROCLINK_BLOCK_LEN; ++i) b[i] = (uint8_t)((b[i] << 1) | (b[i + 1u] >> 7));
    b[ROCLINK_BLOCK_LEN - 1u] = (uint8_t)(b[ROCLINK_BLOCK_LEN - 1u] << 1);
    if (carry) b[ROCLINK_BLOCK_LEN - 1u] = (uint8_t)(b[ROCLINK_BLOCK_LEN - 1u] ^ 0x87u);
}

int roclink_crypto_cmac(const uint8_t key[ROCLINK_KEY_LEN], const uint8_t *msg, size_t len,
                        uint8_t mac[ROCLINK_BLOCK_LEN]) {
    uint8_t k[ROCLINK_BLOCK_LEN], x[ROCLINK_BLOCK_LEN], last[ROCLINK_BLOCK_LEN];
    size_t full, i, rest;
    int rc;
    if (!key || !mac || (len && !msg)) return -1;

    /* K1 = dbl(AES(K, 0)). */
    memset(k, 0, sizeof(k));
    rc = roclink_aes128_encrypt(key, k, k);
    dbl(k);

    /* Every block but the last goes through plain CBC-MAC. */
    full = len ? (len - 1u) / ROCLINK_BLOCK_LEN : 0u;
    memset(x, 0, sizeof(x));
    for (i = 0; i < full && rc == 0; ++i) {
        size_t j;
        for (j = 0; j < ROCLINK_BLOCK_LEN; ++j) x[j] = (uint8_t)(x[j] ^ msg[i * ROCLINK_BLOCK_LEN + j]);
        rc = roclink_aes128_encrypt(key, x, x);
    }

    /* The last block: complete, XOR K1; short (or empty), pad 10..0 and XOR K2. */
    rest = len - full * ROCLINK_BLOCK_LEN;
    memset(last, 0, sizeof(last));
    if (rest) memcpy(last, msg + full * ROCLINK_BLOCK_LEN, rest);
    if (rest < ROCLINK_BLOCK_LEN) {
        last[rest] = 0x80u;
        dbl(k); /* K2 */
    }
    for (i = 0; i < ROCLINK_BLOCK_LEN; ++i) x[i] = (uint8_t)(x[i] ^ last[i] ^ k[i]);
    if (rc == 0) rc = roclink_aes128_encrypt(key, x, mac);

    roclink_crypto_wipe(k, sizeof(k));
    roclink_crypto_wipe(x, sizeof(x));
    roclink_crypto_wipe(last, sizeof(last));
    return rc == 0 ? 0 : -1;
}

/*
 * The one key-derivation function every roclink key
 * uses (NIST SP 800-108 counter mode, PRF = AES-CMAC, one 128-bit block):
 *
 *   KDF(key, label, context) = AES-CMAC(key, 0x01 || label || 0x00 || context || 0x00 0x80)
 */
static roclink_status_t roclink_kdf(const uint8_t key[ROCLINK_KEY_LEN], const char *label,
                                           size_t label_len, const uint8_t *ctx, size_t ctx_len,
                                           uint8_t out[ROCLINK_KEY_LEN]) {
    uint8_t msg[1 + 16 + 1 + 8 + 2];
    size_t i = 0;
    if (label_len > 16 || ctx_len > 8) return ROCLINK_E_ARG;
    msg[i++] = 0x01;
    memcpy(msg + i, label, label_len);
    i += label_len;
    msg[i++] = 0x00;
    memcpy(msg + i, ctx, ctx_len);
    i += ctx_len;
    msg[i++] = 0x00;
    msg[i++] = 0x80;
    return roclink_crypto_cmac(key, msg, i, out) == 0 ? ROCLINK_OK : ROCLINK_E_CRYPTO;
}

/* ======================================================================
 * 2. Security
 * ====================================================================== */

#define PEER_HAVE_FCNT 0x01u /* last_fcnt and last_tag are valid */
#define PEER_HAVE_KEY 0x02u  /* chain_key, epoch and key_gen are valid */
#define PEER_HAVE_FK 0x04u   /* frame_key is FK(epoch) of chain_key */
#define BINDING_LEN 4u
/* The AAD buffer holds only the headers and the binding. Under level auth the
 * payload is authenticated too, but it's handed to CCM in place, as a second
 * piece of AAD, so no copy of it is made (that matters on AVR's 2 KB of RAM). */
#define AAD_MAX (ROCLINK_HEADER_LEN + ROCLINK_SEC_HEADER_LEN + BINDING_LEN)

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#define wipe roclink_crypto_wipe

/* --------------------------------------------------------------- config */

void roclink_sec_config_default(roclink_sec_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->level = ROCLINK_SEC_ENC;
    cfg->tag_len = 8;
    cfg->policy = ROCLINK_POLICY_REQUIRE_ENC;
    cfg->min_tag_len = 8;
    cfg->admission = ROCLINK_ADMIT_OPEN;
    cfg->max_epoch_skip = ROCLINK_EPOCH_SKIP_DEFAULT;
}

static bool valid_level(roclink_sec_level_t l) {
    return l == ROCLINK_SEC_NONE || l == ROCLINK_SEC_AUTH || l == ROCLINK_SEC_ENC;
}

/* Network-wide policies; DENY only exists as a per-node override, and
 * ALLOW_NONE only where it can't expose a secured device (PROTOCOL.md 10.1):
 * an open net at a gateway, or a node that is itself unsecured. */
static bool valid_policy(const roclink_sec_config_t *cfg, bool gateway) {
    switch (cfg->policy) {
        case ROCLINK_POLICY_REQUIRE_ENC:
        case ROCLINK_POLICY_REQUIRE_AUTH: return true;
        case ROCLINK_POLICY_ALLOW_NONE: return gateway ? cfg->open_net : cfg->level == ROCLINK_SEC_NONE;
        default: return false;
    }
}

/* The shortest tag accepted from this peer. */
static size_t min_tag(const roclink_sec_t *s, const roclink_sec_peer_t *peer) {
    if (peer && (peer->min_tag == 4u || peer->min_tag == 8u)) return peer->min_tag;
    return s->cfg.min_tag_len;
}

static bool policy_accepts(unsigned policy, roclink_sec_level_t level) {
    switch (policy) {
        case ROCLINK_POLICY_REQUIRE_ENC: return level == ROCLINK_SEC_ENC;
        case ROCLINK_POLICY_REQUIRE_AUTH: return level == ROCLINK_SEC_ENC || level == ROCLINK_SEC_AUTH;
        case ROCLINK_POLICY_ALLOW_NONE: return true;
        default: return false;
    }
}

/* ----------------------------------------------------------------- keys */

/* net_id | node (LE) | key_gen [| epoch (LE)] */
static size_t key_context(uint8_t c[8], uint8_t net_id, uint16_t node, uint8_t key_gen,
                          const uint32_t *epoch) {
    c[0] = net_id;
    c[1] = (uint8_t)(node & 0xFFu);
    c[2] = (uint8_t)(node >> 8);
    c[3] = key_gen;
    if (!epoch) return 4;
    put_le32(c + 4, *epoch);
    return 8;
}

#define LABEL_NODE "roclink node key" /* 16 */
#define LABEL_CHAIN "roclink chain"   /* 13 */
#define LABEL_FRAME "roclink frame"   /* 13 */

roclink_status_t roclink_sec_derive_node_key(const uint8_t master[ROCLINK_KEY_LEN], uint8_t net_id,
                                             uint16_t node, uint8_t key_gen,
                                             uint8_t node_key[ROCLINK_KEY_LEN]) {
    uint8_t c[8];
    size_t n;
    if (!master || !node_key || !roclink_is_node_addr(node) || key_gen > ROCLINK_SECCTRL_KEY_GEN_MASK) {
        return ROCLINK_E_ARG;
    }
    n = key_context(c, net_id, node, key_gen, NULL);
    return roclink_kdf(master, LABEL_NODE, 16, c, n, node_key);
}

roclink_status_t roclink_sec_chain_advance(const uint8_t chain_key[ROCLINK_KEY_LEN], uint8_t net_id,
                                           uint16_t node, uint8_t key_gen, uint32_t from,
                                           uint32_t to, uint8_t out[ROCLINK_KEY_LEN]) {
    uint8_t cur[ROCLINK_KEY_LEN], c[8];
    roclink_status_t st = ROCLINK_OK;
    uint32_t e;
    if (!chain_key || !out || to < from || to > ROCLINK_EPOCH_MAX) return ROCLINK_E_ARG;
    if (to - from > ROCLINK_MAX_EPOCH_SKIP) return ROCLINK_E_KEY;
    memcpy(cur, chain_key, ROCLINK_KEY_LEN);
    for (e = from; e < to && st == ROCLINK_OK; ++e) {
        const uint32_t next = e + 1u;
        const size_t n = key_context(c, net_id, node, key_gen, &next);
        st = roclink_kdf(cur, LABEL_CHAIN, 13, c, n, cur); /* CMAC reads all input before writing */
    }
    if (st == ROCLINK_OK) memcpy(out, cur, ROCLINK_KEY_LEN);
    wipe(cur, sizeof(cur));
    return st;
}

static roclink_status_t frame_key_of(const uint8_t chain_key[ROCLINK_KEY_LEN], uint8_t net_id,
                                     uint16_t node, uint8_t key_gen, uint32_t epoch,
                                     uint8_t out[ROCLINK_KEY_LEN]) {
    uint8_t c[8];
    const size_t n = key_context(c, net_id, node, key_gen, &epoch);
    return roclink_kdf(chain_key, LABEL_FRAME, 13, c, n, out);
}

/* --------------------------------------------------------- frame counter */

void roclink_fcnt_init(roclink_fcnt_t *c, uint32_t stored, uint32_t block) {
    if (!c) return;
    c->next = stored;
    c->limit = stored; /* nothing reserved yet: the first take asks for a persist */
    c->block = block ? block : ROCLINK_FCNT_BLOCK_DEFAULT;
}

roclink_status_t roclink_fcnt_take(roclink_fcnt_t *c, uint32_t *fcnt) {
    if (!c || !fcnt) return ROCLINK_E_ARG;
    if (c->next == 0xFFFFFFFFu) return ROCLINK_E_EXHAUSTED; /* last value never used */
    if (c->next >= c->limit) return ROCLINK_E_PERSIST;
    *fcnt = c->next++;
    return ROCLINK_OK;
}

uint32_t roclink_fcnt_to_persist(const roclink_fcnt_t *c) {
    if (!c) return 0;
    return c->block > 0xFFFFFFFFu - c->next ? 0xFFFFFFFFu : c->next + c->block;
}

void roclink_fcnt_persisted(roclink_fcnt_t *c, uint32_t value) {
    if (c && value > c->limit) c->limit = value;
}

roclink_status_t roclink_fcnt_next_epoch(roclink_fcnt_t *c) {
    uint32_t e;
    if (!c) return ROCLINK_E_ARG;
    e = ROCLINK_EPOCH_OF(c->next);
    if (e >= ROCLINK_EPOCH_MAX) return ROCLINK_E_EXHAUSTED;
    c->next = (e + 1u) << ROCLINK_EPOCH_SHIFT;
    return ROCLINK_OK;
}

roclink_status_t roclink_fcnt_skip(roclink_fcnt_t *c, uint32_t n) {
    if (!c) return ROCLINK_E_ARG;
    if (n >= 0xFFFFFFFFu - c->next) {
        c->next = 0xFFFFFFFFu; /* the last value is never used: exhausted */
        return ROCLINK_E_EXHAUSTED;
    }
    c->next += n; /* past the reservation, so the next take asks for a persist */
    return ROCLINK_OK;
}

/* ------------------------------------------------------------ peer state */

roclink_status_t roclink_sec_peer_enroll(roclink_sec_peer_t *peer,
                                         const uint8_t chain_key[ROCLINK_KEY_LEN], uint8_t key_gen,
                                         uint32_t epoch) {
    uint8_t policy;
    if (!peer || !chain_key || key_gen > ROCLINK_SECCTRL_KEY_GEN_MASK || epoch > ROCLINK_EPOCH_MAX) {
        return ROCLINK_E_ARG;
    }
    policy = peer->policy;
    wipe(peer, sizeof(*peer));
    peer->policy = policy;
    memcpy(peer->chain_key, chain_key, ROCLINK_KEY_LEN);
    peer->epoch = epoch;
    peer->key_gen = key_gen;
    peer->state = PEER_HAVE_KEY;
    return ROCLINK_OK;
}

/* --------------------------------------------------------------- context */

static roclink_status_t init_common(roclink_sec_t *s, const roclink_sec_config_t *cfg,
                                    const uint8_t key[ROCLINK_KEY_LEN], uint8_t key_gen,
                                    bool gateway) {
    if (!s || !cfg || !valid_level(cfg->level) || !valid_policy(cfg, gateway) ||
        (cfg->tag_len != 4 && cfg->tag_len != 8) || key_gen > ROCLINK_SECCTRL_KEY_GEN_MASK ||
        (cfg->min_tag_len != 0 && cfg->min_tag_len != 4 && cfg->min_tag_len != 8)) {
        return ROCLINK_E_ARG;
    }
    if (gateway ? !roclink_is_gateway_addr(cfg->addr) : !roclink_is_node_addr(cfg->addr)) {
        return ROCLINK_E_ARG;
    }
    if (gateway && ((cfg->admission != ROCLINK_ADMIT_OPEN && cfg->admission != ROCLINK_ADMIT_LISTED) ||
                    cfg->max_epoch_skip > ROCLINK_MAX_EPOCH_SKIP)) {
        return ROCLINK_E_ARG;
    }
    /* A node runs without a key only as an explicit, complete opt-out. A
     * gateway may run without the master: its nodes are enrolled instead. */
    if (!gateway && !key && (cfg->level != ROCLINK_SEC_NONE || cfg->policy != ROCLINK_POLICY_ALLOW_NONE)) {
        return ROCLINK_E_ARG;
    }
    /* Anything that will use keys checks the AES provider once. */
    if ((key || (gateway && !cfg->open_net)) && roclink_crypto_selftest() != 0) return ROCLINK_E_CRYPTO;
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    if (s->cfg.max_epoch_skip == 0) s->cfg.max_epoch_skip = ROCLINK_EPOCH_SKIP_DEFAULT;
    if (s->cfg.min_tag_len == 0) s->cfg.min_tag_len = 8;
    if (gateway) s->cfg.always_listening = false;
    else s->cfg.open_net = false;
    s->is_gateway = gateway;
    s->key_gen[0] = key_gen;
    if (key) {
        memcpy(s->key[0], key, ROCLINK_KEY_LEN);
        s->key_count = 1;
    }
    return ROCLINK_OK;
}

roclink_status_t roclink_sec_init_node_at(roclink_sec_t *s, const roclink_sec_config_t *cfg,
                                          const uint8_t chain_key[ROCLINK_KEY_LEN], uint8_t key_gen,
                                          uint32_t epoch) {
    roclink_status_t st;
    if (epoch > ROCLINK_EPOCH_MAX) return ROCLINK_E_ARG;
    st = init_common(s, cfg, chain_key, key_gen, false);
    if (st != ROCLINK_OK || !chain_key) return st;
    s->epoch = epoch;
    st = frame_key_of(s->key[0], cfg->net_id, cfg->addr, key_gen, epoch, s->frame_key);
    if (st != ROCLINK_OK) roclink_sec_wipe(s);
    return st;
}

roclink_status_t roclink_sec_init_node(roclink_sec_t *s, const roclink_sec_config_t *cfg,
                                       const uint8_t chain_key[ROCLINK_KEY_LEN], uint8_t key_gen) {
    return roclink_sec_init_node_at(s, cfg, chain_key, key_gen, 0);
}

roclink_status_t roclink_sec_init_gateway(roclink_sec_t *s, const roclink_sec_config_t *cfg,
                                          const uint8_t master_key[ROCLINK_KEY_LEN],
                                          uint8_t key_gen) {
    return init_common(s, cfg, master_key, key_gen, true);
}

roclink_status_t roclink_sec_add_key(roclink_sec_t *s, const uint8_t key[ROCLINK_KEY_LEN],
                                     uint8_t key_gen) {
    /* Only the generation before the current one: the current one is always
     * the one nodes are moving to (PROTOCOL.md 10.4). */
    if (!s || !s->is_gateway || !key || s->key_count != 1 ||
        key_gen != ((s->key_gen[0] + ROCLINK_SECCTRL_KEY_GEN_MASK) & ROCLINK_SECCTRL_KEY_GEN_MASK)) {
        return ROCLINK_E_ARG;
    }
    memcpy(s->key[1], key, ROCLINK_KEY_LEN);
    s->key_gen[1] = key_gen;
    s->key_count = 2;
    return ROCLINK_OK;
}

void roclink_sec_wipe(roclink_sec_t *s) {
    if (!s) return;
    wipe(s->key, sizeof(s->key));
    wipe(s->frame_key, sizeof(s->frame_key));
    wipe(s->pending_key, sizeof(s->pending_key));
    s->key_pending = false;
}

roclink_status_t roclink_sec_peer_admit(const roclink_sec_t *s, roclink_sec_peer_t *peer,
                                        uint16_t node, uint32_t epoch) {
    uint8_t ck0[ROCLINK_KEY_LEN], ck[ROCLINK_KEY_LEN];
    roclink_status_t st;
    if (!s || !s->is_gateway || !s->key_count || !peer || epoch > ROCLINK_EPOCH_MAX) {
        return ROCLINK_E_ARG;
    }
    st = roclink_sec_derive_node_key(s->key[0], s->cfg.net_id, node, s->key_gen[0], ck0);
    if (st == ROCLINK_OK) st = roclink_sec_chain_advance(ck0, s->cfg.net_id, node, s->key_gen[0], 0, epoch, ck);
    if (st == ROCLINK_OK) st = roclink_sec_peer_enroll(peer, ck, s->key_gen[0], epoch);
    if (st == ROCLINK_OK &&
        frame_key_of(ck, s->cfg.net_id, node, s->key_gen[0], epoch, peer->frame_key) == ROCLINK_OK) {
        peer->state = (uint8_t)(peer->state | PEER_HAVE_FK); /* else it's derived on first use */
    }
    wipe(ck0, sizeof(ck0));
    wipe(ck, sizeof(ck));
    return st;
}

void roclink_sec_close_window(roclink_sec_t *s) {
    if (!s || s->is_gateway || s->cfg.always_listening) return;
    s->have_opener = false;
    s->reply_count = 0;
}

/* -------------------------------------------------------- peer records */

static void put_rec32(uint8_t **p, uint32_t v) {
    put_le32(*p, v);
    *p += 4;
}

void roclink_sec_peer_save(const roclink_sec_peer_t *peer, uint8_t out[ROCLINK_SEC_PEER_RECORD_LEN]) {
    uint8_t *p = out;
    if (!peer || !out) return;
    *p++ = 1u; /* format version */
    put_rec32(&p, peer->last_fcnt);
    put_rec32(&p, peer->epoch);
    memcpy(p, peer->chain_key, ROCLINK_KEY_LEN);
    p += ROCLINK_KEY_LEN;
    memcpy(p, peer->frame_key, ROCLINK_KEY_LEN);
    p += ROCLINK_KEY_LEN;
    memcpy(p, peer->last_tag, sizeof(peer->last_tag));
    p += sizeof(peer->last_tag);
    *p++ = peer->key_gen;
    *p++ = peer->state;
    *p++ = peer->policy;
    *p = peer->min_tag;
}

roclink_status_t roclink_sec_peer_load(roclink_sec_peer_t *peer,
                                       const uint8_t in[ROCLINK_SEC_PEER_RECORD_LEN]) {
    const uint8_t *p = in;
    if (!peer || !in || in[0] != 1u) return ROCLINK_E_ARG;
    ++p;
    memset(peer, 0, sizeof(*peer));
    peer->last_fcnt = get_le32(p);
    peer->epoch = get_le32(p + 4);
    p += 8;
    memcpy(peer->chain_key, p, ROCLINK_KEY_LEN);
    p += ROCLINK_KEY_LEN;
    memcpy(peer->frame_key, p, ROCLINK_KEY_LEN);
    p += ROCLINK_KEY_LEN;
    memcpy(peer->last_tag, p, sizeof(peer->last_tag));
    p += sizeof(peer->last_tag);
    peer->key_gen = p[0];
    peer->state = p[1];
    peer->policy = p[2];
    peer->min_tag = p[3];
    return ROCLINK_OK;
}

bool roclink_sec_key_pending(const roclink_sec_t *s, uint8_t chain_key[ROCLINK_KEY_LEN],
                             uint32_t *epoch) {
    if (!s || !s->key_pending) return false;
    if (chain_key) memcpy(chain_key, s->pending_key, ROCLINK_KEY_LEN);
    if (epoch) *epoch = s->pending_epoch;
    return true;
}

roclink_status_t roclink_sec_key_persisted(roclink_sec_t *s, uint32_t epoch) {
    uint8_t fk[ROCLINK_KEY_LEN];
    roclink_status_t st;
    if (!s || s->is_gateway || !s->key_pending || epoch != s->pending_epoch) return ROCLINK_E_ARG;
    st = frame_key_of(s->pending_key, s->cfg.net_id, s->cfg.addr, s->key_gen[0], epoch, fk);
    if (st != ROCLINK_OK) return st;
    memcpy(s->key[0], s->pending_key, ROCLINK_KEY_LEN); /* the old chain key is gone from here on */
    memcpy(s->frame_key, fk, ROCLINK_KEY_LEN);
    wipe(fk, sizeof(fk));
    wipe(s->pending_key, sizeof(s->pending_key));
    s->epoch = epoch;
    s->key_pending = false;
    s->have_opener = false;
    s->reply_count = 0;
    return ROCLINK_OK;
}

/* Index of master generation `gen`, or -1. */
static int find_key(const roclink_sec_t *s, uint8_t gen) {
    uint8_t i;
    for (i = 0; i < s->key_count; ++i) {
        if (s->key_gen[i] == gen) return (int)i;
    }
    return -1;
}

/* Gateway: the chain key for `node` under `gen` at `epoch`, from the peer's
 * stored chain if it has one for that generation, otherwise from the master.
 * Earlier epochs than the stored one are gone (ROCLINK_E_REPLAY). */
static roclink_status_t gw_chain(const roclink_sec_t *s, const roclink_sec_peer_t *peer,
                                 uint16_t node, uint8_t gen, uint32_t epoch,
                                 uint8_t out[ROCLINK_KEY_LEN]) {
    uint8_t ck0[ROCLINK_KEY_LEN];
    roclink_status_t st;
    int idx;
    if ((peer->state & PEER_HAVE_KEY) && peer->key_gen == gen) {
        if (epoch < peer->epoch) return ROCLINK_E_REPLAY;
        return roclink_sec_chain_advance(peer->chain_key, s->cfg.net_id, node, gen, peer->epoch,
                                         epoch, out);
    }
    idx = find_key(s, gen);
    if (idx < 0) return ROCLINK_E_KEY; /* unknown generation, or no master and not enrolled */
    st = roclink_sec_derive_node_key(s->key[idx], s->cfg.net_id, node, gen, ck0);
    if (st == ROCLINK_OK) st = roclink_sec_chain_advance(ck0, s->cfg.net_id, node, gen, 0, epoch, out);
    wipe(ck0, sizeof(ck0));
    return st;
}

/* The peer's cached frame key is the one for `gen` at `epoch`. */
static bool fk_cached(const roclink_sec_peer_t *peer, uint8_t gen, uint32_t epoch) {
    return (peer->state & (PEER_HAVE_KEY | PEER_HAVE_FK)) == (PEER_HAVE_KEY | PEER_HAVE_FK) &&
           peer->key_gen == gen && peer->epoch == epoch;
}

/*
 * Gateway, before any cryptography: may a frame from this node under `gen`
 * at `epoch` be opened, and how many key derivations will that take? Applies
 * admission and the epoch-skip limits (PROTOCOL.md 10.4), so a forged frame's
 * cost is bounded before it is paid.
 */
static roclink_status_t gw_plan(const roclink_sec_t *s, const roclink_sec_peer_t *peer, uint8_t gen,
                                uint32_t epoch, unsigned *kdfs) {
    uint32_t limit;
    if ((peer->state & PEER_HAVE_KEY) && peer->key_gen == gen) { /* from the stored chain */
        if (epoch < peer->epoch) return ROCLINK_E_REPLAY;          /* earlier keys are gone */
        if (epoch - peer->epoch > s->cfg.max_epoch_skip) return ROCLINK_E_EPOCH;
        *kdfs = fk_cached(peer, gen, epoch) ? 0u : (unsigned)(epoch - peer->epoch) + 1u;
        return ROCLINK_OK;
    }
    /* From the master: first contact, or a new master generation. */
    if (!(peer->state & PEER_HAVE_KEY) && (!s->key_count || s->cfg.admission != ROCLINK_ADMIT_OPEN)) {
        return ROCLINK_E_NOT_ENROLLED;
    }
    if (find_key(s, gen) < 0) return ROCLINK_E_KEY;
    limit = s->cfg.admission == ROCLINK_ADMIT_OPEN ? ROCLINK_MAX_EPOCH_SKIP : s->cfg.max_epoch_skip;
    if (epoch > limit) return ROCLINK_E_EPOCH;
    *kdfs = (unsigned)epoch + 2u; /* CK(0), the ratchet, FK(epoch) */
    return ROCLINK_OK;
}

/* ------------------------------------------------------------- nonce, AAD */

/* src (LE) | dst (LE) | net_id | fcnt (LE) | 00 00 00 00 */
static void make_nonce(uint8_t n[ROCLINK_NONCE_LEN], const uint8_t *frame, uint32_t fcnt) {
    memcpy(n, frame + 3, 4); /* src and dst, already little-endian on the wire */
    n[4] = frame[2];
    put_le32(n + 5, fcnt);
    memset(n + 9, 0, 4);
}

/*
 * base header (relay fields masked) | sec_ctrl | fcnt | [binding]
 * `sec` points at the 5-byte security header in the frame. Under level auth
 * the payload follows as the AAD's second piece (see AAD_MAX).
 */
static size_t make_aad(uint8_t *aad, const uint8_t *frame, const uint8_t *sec,
                       const uint32_t *binding) {
    size_t n = ROCLINK_HEADER_LEN;
    memcpy(aad, frame, ROCLINK_HEADER_LEN);
    aad[1] = (uint8_t)(aad[1] & ~(ROCLINK_FLAG_RELAYED | ROCLINK_FLAG_HOPS_MASK));
    memcpy(aad + n, sec, ROCLINK_SEC_HEADER_LEN);
    n += ROCLINK_SEC_HEADER_LEN;
    if (binding) {
        put_le32(aad + n, *binding);
        n += BINDING_LEN;
    }
    return n;
}

static bool is_reply_opener(const roclink_header_t *h) {
    return h->type == ROCLINK_TYPE_DATA &&
           (h->flags & (ROCLINK_FLAG_LISTEN | ROCLINK_FLAG_ACK_REQ)) != 0;
}

/* ------------------------------------------------------------------ seal */

/* Node: before taking a counter, make sure the key matches its epoch. */
static roclink_status_t node_prepare_epoch(roclink_sec_t *s, roclink_fcnt_t *ctr) {
    uint32_t want;
    roclink_status_t st;
    if (s->key_pending) return ROCLINK_E_PERSIST_KEY;
    if (ctr->next == 0xFFFFFFFFu) return ROCLINK_E_EXHAUSTED;
    if (ROCLINK_EPOCH_OF(ctr->next) < s->epoch) {
        ctr->next = s->epoch << ROCLINK_EPOCH_SHIFT; /* earlier epochs' keys are gone: skip ahead */
    }
    want = ROCLINK_EPOCH_OF(ctr->next);
    if (want == s->epoch) return ROCLINK_OK;
    st = roclink_sec_chain_advance(s->key[0], s->cfg.net_id, s->cfg.addr, s->key_gen[0], s->epoch,
                                   want, s->pending_key);
    if (st != ROCLINK_OK) return st;
    s->pending_epoch = want;
    s->key_pending = true;
    return ROCLINK_E_PERSIST_KEY;
}

int roclink_sec_seal(roclink_sec_t *s, roclink_fcnt_t *ctr, const roclink_sec_info_t *opener,
                     const roclink_sec_peer_t *peer, const uint8_t *in, size_t len, uint8_t *out,
                     size_t out_cap) {
    roclink_header_t h;
    const uint8_t *payload;
    size_t plen, hl, tag, total, aad_len;
    roclink_sec_level_t level;
    uint8_t ck[ROCLINK_KEY_LEN];
    uint8_t key[ROCLINK_KEY_LEN];
    uint8_t nonce[ROCLINK_NONCE_LEN];
    uint8_t aad[AAD_MAX];
    uint8_t *sec;
    uint8_t gen;
    uint32_t fcnt;
    roclink_status_t st;
    int rc;

    if (!s || !in || !out) return ROCLINK_E_ARG;
    st = roclink_decode(in, len, &h, &payload, &plen);
    if (st != ROCLINK_OK) return st;
    if (h.flags & ROCLINK_FLAG_SECURED) return ROCLINK_E_ARG; /* already sealed */
    if (s->is_gateway && !opener) return ROCLINK_E_ARG;       /* gateway frames are replies */
    if (peer && peer->policy == ROCLINK_POLICY_DENY) return ROCLINK_E_REVOKED;

    level = s->cfg.level;
    if (s->is_gateway && opener->level == ROCLINK_SEC_NONE) level = ROCLINK_SEC_NONE;

    if (level == ROCLINK_SEC_NONE) {
        if (len > out_cap) return ROCLINK_E_NOSPACE;
        if (out != in) memcpy(out, in, len);
        if (!s->is_gateway && h.type == ROCLINK_TYPE_DATA) s->have_opener = false;
        return (int)len;
    }

    hl = roclink_header_len(h.flags);
    tag = s->cfg.tag_len;
    total = len + ROCLINK_SEC_OVERHEAD(tag);
    if (total > ROCLINK_FRAME_MAX_LEN) return ROCLINK_E_TOO_LONG;
    if (level == ROCLINK_SEC_AUTH && total > ROCLINK_STD_FRAME_MAX_LEN) return ROCLINK_E_TOO_LONG;
    if (total > out_cap) return ROCLINK_E_NOSPACE;
    if (!ctr || (s->is_gateway && !peer)) return ROCLINK_E_ARG;
    if (h.src != s->cfg.addr || (s->is_gateway && !roclink_is_node_addr(h.dst))) {
        return ROCLINK_E_ADDR; /* only our own frames, and gateways only send to nodes */
    }

    /* The frame key: the node's current one, or the gateway's for that node at
     * the opener's epoch (both directions of an exchange use the node's chain). */
    if (s->is_gateway) {
        const uint32_t epoch = ROCLINK_EPOCH_OF(opener->fcnt);
        gen = (peer->state & PEER_HAVE_KEY) ? peer->key_gen : s->key_gen[0];
        if (fk_cached(peer, gen, epoch)) {
            memcpy(key, peer->frame_key, ROCLINK_KEY_LEN);
        } else {
            st = gw_chain(s, peer, h.dst, gen, epoch, ck);
            if (st == ROCLINK_OK) st = frame_key_of(ck, s->cfg.net_id, h.dst, gen, epoch, key);
        }
        wipe(ck, sizeof(ck));
        if (st != ROCLINK_OK) {
            wipe(key, sizeof(key));
            return st;
        }
    } else {
        gen = s->key_gen[0];
        st = node_prepare_epoch(s, ctr);
        if (st != ROCLINK_OK) return st;
        memcpy(key, s->frame_key, ROCLINK_KEY_LEN);
    }

    st = roclink_fcnt_take(ctr, &fcnt); /* last check: a failure after this wastes a counter */
    if (st != ROCLINK_OK) {
        wipe(key, sizeof(key));
        return st;
    }

    /* Lay out header | sec_ctrl | fcnt | payload, then encrypt/tag in place. */
    if (out == in) {
        memmove(out + hl + ROCLINK_SEC_HEADER_LEN, out + hl, plen);
    } else {
        memcpy(out, in, hl);
        memcpy(out + hl + ROCLINK_SEC_HEADER_LEN, in + hl, plen);
    }
    out[1] = (uint8_t)(out[1] | ROCLINK_FLAG_SECURED);
    sec = out + hl;
    sec[0] = (uint8_t)((ROCLINK_SECCTRL_SUITE_AES128_CCM << ROCLINK_SECCTRL_SUITE_SHIFT) |
                       (level == ROCLINK_SEC_ENC ? ROCLINK_SECCTRL_ENC : 0u) |
                       (tag == 8 ? ROCLINK_SECCTRL_TAG8 : 0u) | gen);
    put_le32(sec + 1, fcnt);
    payload = sec + ROCLINK_SEC_HEADER_LEN;

    aad_len = make_aad(aad, out, sec, s->is_gateway ? &opener->fcnt : NULL);
    make_nonce(nonce, out, fcnt);
    rc = level == ROCLINK_SEC_ENC
             ? roclink_crypto_ccm_seal(key, nonce, aad, aad_len, NULL, 0,
                                       sec + ROCLINK_SEC_HEADER_LEN, plen, tag)
             : roclink_crypto_ccm_seal(key, nonce, aad, aad_len, payload, plen,
                                       sec + ROCLINK_SEC_HEADER_LEN + plen, 0, tag);
    wipe(key, sizeof(key));
    if (rc != 0) return ROCLINK_E_CRYPTO;

    if (!s->is_gateway && h.type == ROCLINK_TYPE_DATA) {
        /* Replies are bound to the frame that opened them (PROTOCOL.md 10.6).
         * An always-listening node binds to every DATA frame it sends. */
        s->have_opener = s->cfg.always_listening || is_reply_opener(&h);
        s->opener_fcnt = fcnt;
        s->reply_count = 0;
    }
    return (int)total;
}

/* ------------------------------------------------------------------ open */

/* Node: which slot tracks gateway `src` under the current binding, or -1. */
static int reply_slot(const roclink_sec_t *s, uint16_t src) {
    uint8_t i;
    for (i = 0; i < s->reply_count; ++i) {
        if (s->reply_src[i] == src) return (int)i;
    }
    return -1;
}

/* What the checks before any cryptography found out about a frame. */
typedef struct {
    roclink_header_t h;
    size_t plen;
    roclink_sec_level_t level;
    uint8_t gen;
    uint32_t epoch;
    bool retransmission;
    unsigned kdfs; /* key derivations the gateway still needs */
} open_plan_t;

/* Steps 1-4 of PROTOCOL.md 10.7: everything that needs no cryptography. */
static roclink_status_t open_plan(const roclink_sec_t *s, const roclink_sec_peer_t *peer,
                                  const uint8_t *frame, size_t len, open_plan_t *p) {
    const uint8_t *payload;
    unsigned policy;
    roclink_status_t st;
    memset(p, 0, sizeof(*p));

    /* 1. Decode and the cheap filters. */
    st = roclink_decode(frame, len, &p->h, &payload, &p->plen);
    if (st != ROCLINK_OK) return st;
    if (p->h.net_id != s->cfg.net_id) return ROCLINK_E_NET;
    if (s->is_gateway) {
        if (!roclink_is_node_addr(p->h.src) ||
            (p->h.dst != s->cfg.addr && p->h.dst != ROCLINK_ADDR_ANY_GATEWAY)) {
            return ROCLINK_E_ADDR;
        }
        if (!peer) return ROCLINK_E_ARG;
    } else if (!roclink_is_gateway_addr(p->h.src) || p->h.dst != s->cfg.addr) {
        return ROCLINK_E_ADDR;
    }
    if (p->h.flags & ROCLINK_FLAG_RELAYED) return ROCLINK_E_RELAYED;

    /* 2. Revocation and policy: the receiver decides, so plaintext can't
     * downgrade a link, and a revoked node is refused at every level. */
    if (peer && peer->policy == ROCLINK_POLICY_DENY) return ROCLINK_E_REVOKED;
    if (!(p->h.flags & ROCLINK_FLAG_SECURED)) {
        p->level = ROCLINK_SEC_NONE;
    } else {
        p->level = (p->h.sec_ctrl & ROCLINK_SECCTRL_ENC) ? ROCLINK_SEC_ENC : ROCLINK_SEC_AUTH;
    }
    policy = (peer && peer->policy) ? peer->policy : (unsigned)s->cfg.policy;
    if (!policy_accepts(policy, p->level)) return ROCLINK_E_POLICY;
    if (p->level == ROCLINK_SEC_NONE) return ROCLINK_OK;
    if (roclink_sec_tag_len(p->h.sec_ctrl) < min_tag(s, peer)) return ROCLINK_E_POLICY;
    if (p->level == ROCLINK_SEC_AUTH && len > ROCLINK_STD_FRAME_MAX_LEN) return ROCLINK_E_TOO_LONG;

    /* 3. Suite and key generation. */
    if (((p->h.sec_ctrl & ROCLINK_SECCTRL_SUITE_MASK) >> ROCLINK_SECCTRL_SUITE_SHIFT) !=
        ROCLINK_SECCTRL_SUITE_AES128_CCM) {
        return ROCLINK_E_KEY;
    }
    p->gen = (uint8_t)(p->h.sec_ctrl & ROCLINK_SECCTRL_KEY_GEN_MASK);

    /* 4. Freshness, admission and the ratchet's cost. */
    if (!s->is_gateway) {
        if (p->gen != s->key_gen[0]) return ROCLINK_E_KEY;
        if (!s->have_opener) return ROCLINK_E_UNEXPECTED; /* not listening for a reply */
        {
            const int slot = reply_slot(s, p->h.src);
            if (slot >= 0 && p->h.fcnt <= s->reply_fcnt[slot]) return ROCLINK_E_REPLAY;
            if (slot < 0 && s->reply_count >= ROCLINK_SEC_REPLY_GATEWAYS) {
                return ROCLINK_E_UNEXPECTED; /* too many gateways answering one frame */
            }
        }
        return ROCLINK_OK;
    }
    if ((peer->state & PEER_HAVE_KEY) && peer->key_gen != p->gen && s->key_count &&
        peer->key_gen == s->key_gen[0]) {
        return ROCLINK_E_KEY; /* node already moved to the current generation: no going back */
    }
    if ((peer->state & PEER_HAVE_FCNT) && peer->key_gen == p->gen) {
        if (p->h.fcnt < peer->last_fcnt) return ROCLINK_E_REPLAY;
        p->retransmission = p->h.fcnt == peer->last_fcnt;
    }
    p->epoch = ROCLINK_EPOCH_OF(p->h.fcnt);
    return gw_plan(s, peer, p->gen, p->epoch, &p->kdfs);
}

int roclink_sec_open_cost(const roclink_sec_t *s, const roclink_sec_peer_t *peer,
                          const uint8_t *frame, size_t len) {
    open_plan_t p;
    roclink_status_t st;
    if (!s || !frame) return ROCLINK_E_ARG;
    st = open_plan(s, peer, frame, len, &p);
    return st == ROCLINK_OK ? (int)p.kdfs : st;
}

int roclink_sec_open(roclink_sec_t *s, roclink_sec_peer_t *peer, uint8_t *frame, size_t len,
                     roclink_sec_info_t *info) {
    open_plan_t p;
    size_t hl, tag, aad_len;
    uint8_t ck[ROCLINK_KEY_LEN];
    uint8_t key[ROCLINK_KEY_LEN];
    uint8_t nonce[ROCLINK_NONCE_LEN];
    uint8_t aad[AAD_MAX];
    uint8_t rx_tag[8];
    uint8_t *ct;
    roclink_status_t st;
    int rc;

    if (!s || !frame || !info) return ROCLINK_E_ARG;
    memset(info, 0, sizeof(*info));

    /* 1-4. Everything that needs no cryptography. */
    st = open_plan(s, peer, frame, len, &p);
    if (st != ROCLINK_OK) return st;
    if (p.level == ROCLINK_SEC_NONE) {
        info->level = ROCLINK_SEC_NONE;
        return (int)len;
    }

    /* 5. The key, then verify and decrypt in place. */
    memset(ck, 0, sizeof(ck));
    if (!s->is_gateway) {
        memcpy(key, s->frame_key, ROCLINK_KEY_LEN);
    } else if (fk_cached(peer, p.gen, p.epoch)) {
        memcpy(ck, peer->chain_key, ROCLINK_KEY_LEN); /* the normal case: no derivation */
        memcpy(key, peer->frame_key, ROCLINK_KEY_LEN);
    } else {
        st = gw_chain(s, peer, p.h.src, p.gen, p.epoch, ck); /* ratchets forward, tentatively */
        if (st == ROCLINK_OK) st = frame_key_of(ck, s->cfg.net_id, p.h.src, p.gen, p.epoch, key);
        if (st != ROCLINK_OK) {
            wipe(ck, sizeof(ck));
            wipe(key, sizeof(key));
            return st;
        }
    }
    hl = roclink_header_len(p.h.flags);
    tag = roclink_sec_tag_len(p.h.sec_ctrl);
    ct = frame + hl + ROCLINK_SEC_HEADER_LEN;
    memset(rx_tag, 0, sizeof(rx_tag));
    memcpy(rx_tag, ct + p.plen, tag);
    aad_len = make_aad(aad, frame, frame + hl, s->is_gateway ? NULL : &s->opener_fcnt);
    make_nonce(nonce, frame, p.h.fcnt);
    rc = p.level == ROCLINK_SEC_ENC
             ? roclink_crypto_ccm_open(key, nonce, aad, aad_len, NULL, 0, ct, p.plen, tag)
             : roclink_crypto_ccm_open(key, nonce, aad, aad_len, ct, p.plen, ct + p.plen, 0, tag);
    if (rc != 0) {
        wipe(ck, sizeof(ck));
        wipe(key, sizeof(key));
        return rc == ROCLINK_CCM_BAD_TAG ? ROCLINK_E_AUTH : ROCLINK_E_CRYPTO;
    }

    /* 6. Only now: update state and hand back a plain frame. */
    if (s->is_gateway) {
        if (p.retransmission) {
            /* Authentic, same counter: it must also be the same frame. If not, a
             * second device holds this key (clone) or the node's counter reset. */
            if (memcmp(rx_tag, peer->last_tag, sizeof(rx_tag)) != 0) {
                wipe(ck, sizeof(ck));
                wipe(key, sizeof(key));
                return ROCLINK_E_CONFLICT;
            }
        } else {
            memcpy(peer->chain_key, ck, ROCLINK_KEY_LEN); /* earlier epochs' keys are dropped */
            memcpy(peer->frame_key, key, ROCLINK_KEY_LEN);
            peer->epoch = p.epoch;
            peer->key_gen = p.gen;
            peer->last_fcnt = p.h.fcnt;
            memcpy(peer->last_tag, rx_tag, sizeof(rx_tag));
            peer->state = (uint8_t)(peer->state | PEER_HAVE_KEY | PEER_HAVE_FCNT | PEER_HAVE_FK);
        }
    } else {
        int slot = reply_slot(s, p.h.src);
        if (slot < 0) {
            slot = (int)s->reply_count++; /* open_plan made sure there's room */
            s->reply_src[slot] = p.h.src;
        }
        s->reply_fcnt[slot] = p.h.fcnt;
    }
    wipe(ck, sizeof(ck));
    wipe(key, sizeof(key));
    memmove(frame + hl, ct, p.plen);
    frame[1] = (uint8_t)(frame[1] & ~ROCLINK_FLAG_SECURED);

    info->level = p.level;
    info->key_gen = p.gen;
    info->fcnt = p.h.fcnt;
    info->retransmission = p.retransmission;
    return (int)(hl + p.plen);
}

