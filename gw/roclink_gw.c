/*
 * roclink_gw.c - a roclink gateway for Linux: LoRa through a Semtech packet
 * forwarder (SX1302/SX1303 concentrators), and Wi-Fi nodes over UDP. It
 * speaks GATEWAY_INTERFACE.md to a hub over MQTT.
 *
 *   roclink-gw [-c /etc/roclink-gw/gw.conf]
 *
 *   LoRa nodes ─radio─ lora_pkt_fwd ─Semtech UDP (127.0.0.1:1700)─┐
 *   Wi-Fi nodes ─────────────────────────────── UDP (:3333) ───────┤
 *                                                                  ▼
 *                       roclink-gw: open (verify, decrypt), dedupe, ACK, reply
 *                                                                  │
 *     roclink/in/<net>/<gw>/up | rejected | down_status  ◄────────┤ MQTT
 *     roclink/out/<net>/<gw>/down  ───────────────────────────────►┘
 *
 * Per network (`net =` lines in the config): a roclink server, and for a
 * secured net the security context, its frame counter and every node's peer
 * record, persisted in <state_dir>/net-<id>.state before anything depends on
 * them (PROTOCOL.md 10.5, 10.7). An open net (PROTOCOL.md Annex B) carries
 * unsecured frames only.
 *
 * Downlink comes from the hub already numbered (PROTOCOL.md 9.1). "window"
 * downlink waits for the node's next reply window; "now" downlink goes out at
 * once, bound to the hub's binding (PROTOCOL.md 10.6). One downlink per
 * window, after the ACK; re-offered in later windows until ACKed or expired.
 *
 * Replies over LoRa are scheduled on the concentrator's clock: the first
 * `lora_reply_delay_ms` after the uplink ended, each next one after the
 * previous one's airtime and a gap, so a node that keeps listening (MORE)
 * hears them all.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <mosquitto.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "roclink.h"
#include "roclink_secure.h"

#define MAX_NETS 4
#define MAX_NODES 64
#define MAX_POLICIES 32
#define MAX_DOWNLINKS 128
#define CHEAP_OPEN 2      /* key derivations an open may cost without touching the budget */
#define COSTLY_PER_SEC 2  /* expensive opens allowed per second */
#define REPORT_INTERVAL_MS 10000
#define REPORTS_PER_INTERVAL 10
#define REPLY_GAP_MS 100  /* between frames of one LoRa reply burst */

/* ================================================================ logging */

static int g_verbose;

static void logf_(const char *level, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s ", level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
#define LOG_INFO(...) logf_("[info]", __VA_ARGS__)
#define LOG_WARN(...) logf_("[warn]", __VA_ARGS__)
#define LOG_ERR(...) logf_("[error]", __VA_ARGS__)
#define LOG_DEBUG(...) do { if (g_verbose) logf_("[debug]", __VA_ARGS__); } while (0)

/* ================================================================== time */

static uint32_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static int64_t wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ================================================================ helpers */

static void hex_encode(const uint8_t *b, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2 * i] = d[b[i] >> 4];
        out[2 * i + 1] = d[b[i] & 15];
    }
    out[2 * n] = 0;
}

static int hex_decode(const char *s, uint8_t *out, size_t cap) {
    size_t n = strlen(s);
    if (n % 2 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; ++i) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return (int)(n / 2);
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const uint8_t *in, size_t n, char *out) {
    size_t i, o = 0;
    for (i = 0; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        out[o++] = B64[v >> 18 & 63]; out[o++] = B64[v >> 12 & 63];
        out[o++] = B64[v >> 6 & 63]; out[o++] = B64[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0);
        out[o++] = B64[v >> 18 & 63]; out[o++] = B64[v >> 12 & 63];
        out[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
}

static int b64_decode(const char *s, uint8_t *out, size_t cap) {
    uint32_t v = 0;
    int bits = 0;
    size_t o = 0;
    for (; *s && *s != '='; ++s) {
        const char *p = strchr(B64, *s);
        if (!p) return -1;
        v = v << 6 | (uint32_t)(p - B64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(v >> bits);
        }
    }
    return (int)o;
}

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> 8 * i); }
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') ++s;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
    return s;
}

/* ================================================================ config */

typedef struct {
    uint16_t gateway_id;
    char state_dir[256];
    char mqtt_host[128];
    int mqtt_port;
    char mqtt_user[64];
    char mqtt_password[128];
    char mqtt_client_id[64];
    char semtech_bind[64];
    int semtech_port;
    char udp_bind[64];
    int udp_port;
    double lora_freq_mhz;
    int lora_sf, lora_bw_khz, lora_cr, lora_power, lora_preamble, lora_reply_delay_ms;
} config_t;

typedef struct {
    uint8_t net;
    uint16_t node;
    uint8_t policy;
} policy_t;

static config_t g_cfg = {
    .gateway_id = 0xFF01, .state_dir = "/var/lib/roclink-gw",
    .mqtt_host = "localhost", .mqtt_port = 1883, .mqtt_client_id = "",
    .semtech_bind = "127.0.0.1", .semtech_port = 1700,
    .udp_bind = "0.0.0.0", .udp_port = 3333,
    .lora_freq_mhz = 915.0, .lora_sf = 9, .lora_bw_khz = 125, .lora_cr = 7, .lora_power = 14,
    .lora_preamble = 8, .lora_reply_delay_ms = 150,
};
static policy_t g_policies[MAX_POLICIES];
static size_t g_policy_count;

/* ============================================================== networks */

typedef enum { LINK_NONE = 0, LINK_LORA, LINK_UDP } link_kind_t;

/* Where a node was last heard, so replies and "now" downlink go back the same way. */
typedef struct {
    link_kind_t kind;
    struct sockaddr_in udp; /* LINK_UDP */
    uint32_t tmst;          /* LINK_LORA: concentrator time at the end of the uplink */
    bool tmst_valid;
    float rssi, snr;
    bool have_radio;
} link_t;

typedef struct {
    uint16_t node; /* 0 = free */
    roclink_sec_peer_t sec;
    link_t link;
} node_slot_t;

typedef struct {
    bool used;
    uint8_t id;
    bool secured;
    roclink_server_t srv;
    roclink_peer_t peers[MAX_NODES];
    roclink_sec_t sec;
    roclink_fcnt_t ctr;
    node_slot_t nodes[MAX_NODES];
    char state_path[300];
    uint32_t budget_second;
    unsigned budget_used;
} net_t;

static net_t g_nets[MAX_NETS];

static net_t *find_net(uint8_t id) {
    for (size_t i = 0; i < MAX_NETS; ++i)
        if (g_nets[i].used && g_nets[i].id == id) return &g_nets[i];
    return NULL;
}

static node_slot_t *find_node(net_t *n, uint16_t node) {
    for (size_t i = 0; i < MAX_NODES; ++i)
        if (n->nodes[i].node == node) return &n->nodes[i];
    return NULL;
}

static uint8_t configured_policy(uint8_t net, uint16_t node) {
    for (size_t i = 0; i < g_policy_count; ++i)
        if (g_policies[i].net == net && g_policies[i].node == node) return g_policies[i].policy;
    return 0;
}

/* ------------------------------------------------------------ persistence */

/* <state_dir>/net-XX.state: "RGW1" | fcnt reservation | MAX_NODES x (node | peer record). */
#define SLOT_REC (2u + ROCLINK_SEC_PEER_RECORD_LEN)
#define STATE_LEN (8u + MAX_NODES * SLOT_REC)

static void save_state(net_t *n, uint32_t fcnt_reserved) {
    static uint8_t buf[STATE_LEN];
    char tmp[320];
    memcpy(buf, "RGW1", 4);
    put_u32(buf + 4, fcnt_reserved);
    for (size_t i = 0; i < MAX_NODES; ++i) {
        uint8_t *rec = buf + 8 + i * SLOT_REC;
        put_u16(rec, n->nodes[i].node);
        roclink_sec_peer_save(&n->nodes[i].sec, rec + 2);
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", n->state_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf) || fsync(fd) != 0 ||
        close(fd) != 0 || rename(tmp, n->state_path) != 0) {
        /* Never accept or send what couldn't be persisted (PROTOCOL.md 10.5). */
        LOG_ERR("cannot write %s: %s", n->state_path, strerror(errno));
        exit(1);
    }
    roclink_crypto_wipe(buf, sizeof(buf));
}

static uint32_t g_reserved[MAX_NETS]; /* the counter value last persisted, per net */

static void persist(net_t *n) { save_state(n, g_reserved[n - g_nets]); }

static void load_state(net_t *n) {
    static uint8_t buf[STATE_LEN];
    int fd = open(n->state_path, O_RDONLY);
    bool ok = fd >= 0 && read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf) &&
              memcmp(buf, "RGW1", 4) == 0;
    if (fd >= 0) close(fd);
    if (!ok) {
        LOG_INFO("net 0x%02X: no state at %s, starting fresh", n->id, n->state_path);
        roclink_fcnt_init(&n->ctr, 0, 0);
        return;
    }
    for (size_t i = 0; i < MAX_NODES; ++i) {
        const uint8_t *rec = buf + 8 + i * SLOT_REC;
        n->nodes[i].node = get_u16(rec);
        if (n->nodes[i].node && roclink_sec_peer_load(&n->nodes[i].sec, rec + 2) != ROCLINK_OK)
            n->nodes[i].node = 0;
        if (n->nodes[i].node) n->nodes[i].sec.policy = configured_policy(n->id, n->nodes[i].node);
    }
    g_reserved[n - g_nets] = get_u32(buf + 4);
    roclink_fcnt_init(&n->ctr, g_reserved[n - g_nets], 0);
    /* A state file restored from a backup must not re-use counter values sent
     * after the backup was taken (PROTOCOL.md 10.5): skip well past them. */
    if (roclink_fcnt_skip(&n->ctr, ROCLINK_FCNT_RESTART_SKIP) != ROCLINK_OK) {
        LOG_ERR("net 0x%02X: frame counter exhausted: re-key the network", n->id);
        exit(1);
    }
    roclink_crypto_wipe(buf, sizeof(buf));
    LOG_INFO("net 0x%02X: restored peer records from %s", n->id, n->state_path);
}

/* Seals a gateway frame, persisting the counter reservation when asked to. */
static int seal(net_t *n, uint8_t *frame, int len, size_t cap, const roclink_sec_info_t *opener,
                const roclink_sec_peer_t *peer) {
    if (!n->secured) return len;
    for (;;) {
        int r = roclink_sec_seal(&n->sec, &n->ctr, opener, peer, frame, (size_t)len, frame, cap);
        if (r != ROCLINK_E_PERSIST) return r;
        uint32_t v = roclink_fcnt_to_persist(&n->ctr);
        g_reserved[n - g_nets] = v;
        persist(n);
        roclink_fcnt_persisted(&n->ctr, v);
    }
}

/* ===================================================================== MQTT */

static struct mosquitto *g_mosq;
static bool g_mqtt_up;

typedef struct dl_req {
    struct dl_req *next;
    char *json;
    uint8_t net;
} dl_req_t;

static pthread_mutex_t g_req_lock = PTHREAD_MUTEX_INITIALIZER;
static dl_req_t *g_req_head, *g_req_tail;
static int g_wake[2]; /* self-pipe: the MQTT thread wakes the main loop */

static void publish(uint8_t net, const char *kind, cJSON *obj) {
    char topic[96];
    char *body = cJSON_PrintUnformatted(obj);
    snprintf(topic, sizeof(topic), "roclink/in/%02x/%04x/%s", net, g_cfg.gateway_id, kind);
    int rc = mosquitto_publish(g_mosq, NULL, topic, (int)strlen(body), body, 1, false);
    if (rc != MOSQ_ERR_SUCCESS) LOG_WARN("publish %s: %s", topic, mosquitto_strerror(rc));
    else LOG_DEBUG("-> %s %s", topic, body);
    free(body);
}

static void on_connect(struct mosquitto *m, void *ud, int rc) {
    (void)ud;
    if (rc != 0) {
        LOG_WARN("broker refused the connection: %s", mosquitto_connack_string(rc));
        return;
    }
    char topic[64];
    snprintf(topic, sizeof(topic), "roclink/out/+/%04x/down", g_cfg.gateway_id);
    mosquitto_subscribe(m, NULL, topic, 1);
    g_mqtt_up = true;
    LOG_INFO("connected to mqtt %s:%d, listening on %s", g_cfg.mqtt_host, g_cfg.mqtt_port, topic);
}

static void on_disconnect(struct mosquitto *m, void *ud, int rc) {
    (void)m; (void)ud;
    g_mqtt_up = false;
    if (rc) LOG_WARN("disconnected from mqtt (%s), reconnecting", mosquitto_strerror(rc));
}

/* Runs on libmosquitto's thread: queue the request for the main loop. */
static void on_message(struct mosquitto *m, void *ud, const struct mosquitto_message *msg) {
    (void)m; (void)ud;
    unsigned net;
    if (sscanf(msg->topic, "roclink/out/%2x/", &net) != 1) return;
    dl_req_t *r = calloc(1, sizeof(*r));
    if (!r) return;
    r->json = strndup(msg->payload, (size_t)msg->payloadlen);
    r->net = (uint8_t)net;
    pthread_mutex_lock(&g_req_lock);
    if (g_req_tail) g_req_tail->next = r; else g_req_head = r;
    g_req_tail = r;
    pthread_mutex_unlock(&g_req_lock);
    if (write(g_wake[1], "x", 1) < 0) { /* the loop wakes within a second anyway */ }
}

/* ============================================================ drop reports */

static const char *reason_of(int status) {
    switch (status) {
        case ROCLINK_E_SHORT: case ROCLINK_E_TOO_LONG: case ROCLINK_E_VERSION:
        case ROCLINK_E_TYPE: case ROCLINK_E_MALFORMED: return "malformed";
        case ROCLINK_E_NET: return "net";
        case ROCLINK_E_ADDR: return "addr";
        case ROCLINK_E_RELAYED: return "relayed";
        case ROCLINK_E_UNEXPECTED: return "unexpected";
        case ROCLINK_E_POLICY: return "policy";
        case ROCLINK_E_KEY: return "key";
        case ROCLINK_E_REVOKED: return "revoked";
        case ROCLINK_E_NOT_ENROLLED: return "not_enrolled";
        case ROCLINK_E_EPOCH: return "epoch";
        case ROCLINK_E_REPLAY: return "replay";
        case ROCLINK_E_AUTH: return "auth";
        case ROCLINK_E_CONFLICT: return "conflict";
        case ROCLINK_E_PEERS_FULL: return "peers_full";
        default: return "other";
    }
}

static bool count_only(const char *reason) {
    return !strcmp(reason, "net") || !strcmp(reason, "addr") || !strcmp(reason, "relayed") ||
           !strcmp(reason, "unexpected");
}

/* Bounded reporting (GATEWAY_INTERFACE.md §4 rule 2): in each interval the
 * first REPORTS_PER_INTERVAL drops are reported one by one, the rest counted
 * per reason and reported with their count when the interval ends. */
#define AGG_SLOTS 16
typedef struct {
    char reason[16];
    uint8_t net;
    unsigned count;
    cJSON *sample;
} agg_t;

static agg_t g_agg[AGG_SLOTS];
static uint32_t g_report_window;
static unsigned g_reports_sent;

static cJSON *report_record(int status, const uint8_t *raw, size_t n, const link_t *link) {
    cJSON *o = cJSON_CreateObject();
    char hex[2 * 64 + 1], id[8];
    cJSON_AddNumberToObject(o, "v", 1);
    cJSON_AddStringToObject(o, "kind", "rejected");
    snprintf(id, sizeof(id), "0x%04X", g_cfg.gateway_id);
    cJSON_AddStringToObject(o, "gateway", id);
    cJSON_AddNumberToObject(o, "time", (double)wall_ms());
    cJSON_AddStringToObject(o, "reason", reason_of(status));
    cJSON_AddNumberToObject(o, "count", 1);
    if (n >= 3) cJSON_AddNumberToObject(o, "net", raw[2]);
    if (n >= ROCLINK_HEADER_LEN) {
        snprintf(id, sizeof(id), "0x%04X", raw[3] | raw[4] << 8);
        cJSON_AddStringToObject(o, "src", id);
        cJSON_AddNumberToObject(o, "dst", raw[5] | raw[6] << 8);
        cJSON_AddNumberToObject(o, "seq", raw[7]);
        cJSON_AddNumberToObject(o, "schema", raw[8]);
        cJSON_AddNumberToObject(o, "flags", raw[1]);
        if ((raw[1] & ROCLINK_FLAG_SECURED) && n >= ROCLINK_HEADER_LEN + ROCLINK_SEC_HEADER_LEN) {
            cJSON_AddNumberToObject(o, "fcnt", get_u32(raw + ROCLINK_HEADER_LEN + 1));
            cJSON_AddNumberToObject(o, "key_gen", raw[ROCLINK_HEADER_LEN] & ROCLINK_SECCTRL_KEY_GEN_MASK);
        }
    }
    cJSON_AddNumberToObject(o, "len", (double)n);
    hex_encode(raw, n < 64 ? n : 64, hex);
    cJSON_AddStringToObject(o, "frame", hex);
    cJSON *radio = cJSON_AddObjectToObject(o, "radio");
    cJSON_AddStringToObject(radio, "transport", link->kind == LINK_LORA ? "lora" : "udp");
    if (link->have_radio) {
        cJSON_AddNumberToObject(radio, "rssi", link->rssi);
        cJSON_AddNumberToObject(radio, "snr", link->snr);
    }
    return o;
}

static void flush_reports(void) {
    for (size_t i = 0; i < AGG_SLOTS; ++i) {
        agg_t *a = &g_agg[i];
        if (!a->sample) continue;
        cJSON_ReplaceItemInObject(a->sample, "count", cJSON_CreateNumber(a->count));
        if (count_only(a->reason)) cJSON_DeleteItemFromObject(a->sample, "src");
        publish(a->net, "rejected", a->sample);
        cJSON_Delete(a->sample);
        memset(a, 0, sizeof(*a));
    }
}

static void dropped(int status, const uint8_t *raw, size_t n, const link_t *link) {
    const char *reason = reason_of(status);
    const uint8_t net = n >= 3 ? raw[2] : 0;
    LOG_DEBUG("dropped a %zu-byte frame: %s", n, roclink_status_str(status));
    if (!count_only(reason) && (status == ROCLINK_E_CONFLICT || g_reports_sent < REPORTS_PER_INTERVAL)) {
        if (status == ROCLINK_E_CONFLICT)
            LOG_WARN("CONFLICT: an authentic frame reused a counter (cloned node, or a counter reset)");
        cJSON *o = report_record(status, raw, n, link);
        publish(net, "rejected", o);
        cJSON_Delete(o);
        g_reports_sent++;
        return;
    }
    agg_t *free_slot = NULL;
    for (size_t i = 0; i < AGG_SLOTS; ++i) {
        agg_t *a = &g_agg[i];
        if (a->sample && a->net == net && !strcmp(a->reason, reason)) {
            a->count++;
            return;
        }
        if (!a->sample && !free_slot) free_slot = a;
    }
    if (!free_slot) return; /* counted nowhere: the table is full for this interval */
    snprintf(free_slot->reason, sizeof(free_slot->reason), "%s", reason);
    free_slot->net = net;
    free_slot->count = 1;
    free_slot->sample = report_record(status, raw, n, link);
}

static void poll_reports(uint32_t now) {
    if (now - g_report_window >= REPORT_INTERVAL_MS) {
        flush_reports();
        g_report_window = now;
        g_reports_sent = 0;
    }
}

/* ============================================================== transports */

static int g_semtech_fd = -1, g_udp_fd = -1;
static struct sockaddr_in g_fwd_down; /* where PULL_RESP goes: the forwarder's PULL_DATA address */
static bool g_fwd_known;
static uint8_t g_fwd_ver = 2;

/* LoRa time on air in µs (Semtech AN1200.13), explicit header, CRC on. */
static uint32_t lora_airtime_us(size_t len) {
    const double tsym = (double)(1u << g_cfg.lora_sf) / (g_cfg.lora_bw_khz * 1000.0);
    const int de = tsym > 0.016 ? 1 : 0;
    const int cr = g_cfg.lora_cr - 4; /* 4/5 -> 1 */
    const double num = 8.0 * (double)len - 4.0 * g_cfg.lora_sf + 28 + 16;
    double nsym = ceil(num / (4.0 * (g_cfg.lora_sf - 2 * de))) * (cr + 4);
    if (nsym < 0) nsym = 0;
    const double t = (g_cfg.lora_preamble + 4.25 + 8 + nsym) * tsym;
    return (uint32_t)(t * 1e6);
}

/* Sends one frame over LoRa through the packet forwarder: at concentrator
 * time `tmst` if `timed`, else at once. */
static void lora_send(const uint8_t *frame, size_t len, bool timed, uint32_t tmst) {
    if (!g_fwd_known) {
        LOG_WARN("no packet forwarder has connected yet: can't send over LoRa");
        return;
    }
    char b64[2 * ROCLINK_FRAME_MAX_LEN], datr[16], codr[8];
    uint8_t pkt[1024];
    b64_encode(frame, len, b64);
    snprintf(datr, sizeof(datr), "SF%dBW%d", g_cfg.lora_sf, g_cfg.lora_bw_khz);
    snprintf(codr, sizeof(codr), "4/%d", g_cfg.lora_cr);
    cJSON *root = cJSON_CreateObject();
    cJSON *tx = cJSON_AddObjectToObject(root, "txpk");
    if (timed) {
        cJSON_AddBoolToObject(tx, "imme", false);
        cJSON_AddNumberToObject(tx, "tmst", tmst);
    } else {
        cJSON_AddBoolToObject(tx, "imme", true);
    }
    cJSON_AddNumberToObject(tx, "freq", g_cfg.lora_freq_mhz);
    cJSON_AddNumberToObject(tx, "rfch", 0);
    cJSON_AddNumberToObject(tx, "powe", g_cfg.lora_power);
    cJSON_AddStringToObject(tx, "modu", "LORA");
    cJSON_AddStringToObject(tx, "datr", datr);
    cJSON_AddStringToObject(tx, "codr", codr);
    cJSON_AddBoolToObject(tx, "ipol", false); /* nodes listen with normal IQ, unlike LoRaWAN */
    cJSON_AddNumberToObject(tx, "prea", g_cfg.lora_preamble);
    cJSON_AddBoolToObject(tx, "ncrc", false);
    cJSON_AddNumberToObject(tx, "size", (double)len);
    cJSON_AddStringToObject(tx, "data", b64);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    size_t bl = strlen(body);
    uint16_t token;
    getrandom(&token, sizeof(token), 0);
    pkt[0] = g_fwd_ver;
    memcpy(pkt + 1, &token, 2);
    pkt[3] = 0x03; /* PULL_RESP */
    if (bl < sizeof(pkt) - 4) {
        memcpy(pkt + 4, body, bl);
        sendto(g_semtech_fd, pkt, 4 + bl, 0, (struct sockaddr *)&g_fwd_down, sizeof(g_fwd_down));
        LOG_DEBUG("lora tx %zu bytes %s", len, timed ? "timed" : "now");
    }
    free(body);
}

/* A burst of frames to one node over its link: replies to an uplink, or "now" downlink. */
typedef struct {
    uint8_t frame[ROCLINK_FRAME_MAX_LEN + 32];
    int len;
} out_frame_t;

static void send_burst(const link_t *link, out_frame_t *f, int count, bool reply) {
    uint32_t at = 0;
    bool timed = false;
    if (link->kind == LINK_LORA && reply && link->tmst_valid) {
        timed = true;
        at = link->tmst + (uint32_t)g_cfg.lora_reply_delay_ms * 1000u;
    }
    for (int i = 0; i < count; ++i) {
        if (f[i].len <= 0) continue;
        if (link->kind == LINK_UDP) {
            sendto(g_udp_fd, f[i].frame, (size_t)f[i].len, 0, (const struct sockaddr *)&link->udp,
                   sizeof(link->udp));
        } else if (link->kind == LINK_LORA) {
            lora_send(f[i].frame, (size_t)f[i].len, timed, at);
            at += lora_airtime_us((size_t)f[i].len) + REPLY_GAP_MS * 1000u;
            if (!timed && i + 1 < count) usleep(lora_airtime_us((size_t)f[i].len) + REPLY_GAP_MS * 1000u);
        }
    }
}

/* =============================================================== downlink */

typedef struct {
    bool used;
    char id[64];
    uint8_t net;
    uint16_t node;
    uint8_t schema, seq;
    bool ack;
    bool now;
    bool has_binding;
    uint32_t binding;
    int64_t expires;
    uint8_t payload[ROCLINK_PAYLOAD_MAX];
    size_t len;
    unsigned offers;
    bool awaiting_ack;
    uint32_t order; /* arrival order: a node's downlink goes out oldest first */
} downlink_t;

static downlink_t g_dl[MAX_DOWNLINKS];
static uint32_t g_dl_order;

static void dl_status(downlink_t *d, const char *status, const char *detail) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "v", 1);
    cJSON_AddStringToObject(o, "id", d->id);
    cJSON_AddStringToObject(o, "status", status);
    if (detail) cJSON_AddStringToObject(o, "detail", detail);
    publish(d->net, "down_status", o);
    cJSON_Delete(o);
    LOG_INFO("downlink %s to %02X/%04X seq %u: %s%s%s", d->id, d->net, d->node, d->seq, status,
             detail ? ", " : "", detail ? detail : "");
    d->used = false;
}

/* The oldest downlink for a node, and whether another waits behind it. */
static downlink_t *dl_for(uint8_t net, uint16_t node, bool *more) {
    downlink_t *first = NULL;
    unsigned count = 0;
    for (size_t i = 0; i < MAX_DOWNLINKS; ++i) {
        downlink_t *d = &g_dl[i];
        if (!d->used || d->net != net || d->node != node) continue;
        count++;
        if (!first || d->order - first->order > 0x80000000u) first = d;
    }
    *more = count > 1;
    return first;
}

/* Builds and seals a downlink for this offer. Returns the frame length or < 0. */
static int dl_build(net_t *n, downlink_t *d, bool more, const roclink_sec_info_t *opener,
                    const roclink_sec_peer_t *peer, out_frame_t *out) {
    uint8_t flags = (d->ack ? ROCLINK_FLAG_ACK_REQ : 0) | (more ? ROCLINK_FLAG_DL_QUEUED : 0);
    int len = roclink_server_build_data_seq(&n->srv, d->node, d->schema, d->payload, d->len, flags,
                                            d->seq, out->frame, sizeof(out->frame));
    if (len > 0) len = seal(n, out->frame, len, sizeof(out->frame), opener, peer);
    out->len = len;
    return len;
}

static void dl_sent(downlink_t *d) {
    d->offers++;
    if (d->ack) d->awaiting_ack = true;
    else dl_status(d, "sent", NULL);
}

/* "now" downlink: send at once to a node we've heard, bound to the hub's binding. */
static void dl_send_now(downlink_t *d) {
    net_t *n = find_net(d->net);
    node_slot_t *s = n ? find_node(n, d->node) : NULL;
    if (!s || s->link.kind == LINK_NONE) return; /* not heard yet: offered in its next window */
    roclink_sec_info_t opener = {0};
    if (n->secured) {
        if (!d->has_binding) return;
        opener.level = ROCLINK_SEC_ENC;
        opener.fcnt = d->binding;
    } else {
        opener.level = ROCLINK_SEC_NONE;
    }
    bool more;
    dl_for(d->net, d->node, &more);
    out_frame_t f;
    if (dl_build(n, d, more, n->secured ? &opener : NULL, n->secured ? &s->sec : NULL, &f) < 0) {
        dl_status(d, "expired", roclink_status_str(f.len));
        return;
    }
    send_burst(&s->link, &f, 1, false);
    dl_sent(d);
}

static void dl_request(uint8_t net, const char *json) {
    cJSON *o = cJSON_Parse(json);
    downlink_t tmp = {0}, *d = NULL;
    const cJSON *v;
    const char *err = NULL;
    tmp.net = net;
    if (!o) { LOG_WARN("downlink request isn't JSON"); return; }
    v = cJSON_GetObjectItem(o, "id");
    snprintf(tmp.id, sizeof(tmp.id), "%s", cJSON_IsString(v) ? v->valuestring : "?");
    v = cJSON_GetObjectItem(o, "node");
    if (cJSON_IsString(v)) tmp.node = (uint16_t)strtoul(v->valuestring, NULL, 0);
    else if (cJSON_IsNumber(v)) tmp.node = (uint16_t)v->valuedouble;
    v = cJSON_GetObjectItem(o, "schema");
    tmp.schema = cJSON_IsNumber(v) ? (uint8_t)v->valuedouble : 0;
    v = cJSON_GetObjectItem(o, "seq");
    tmp.seq = cJSON_IsNumber(v) ? (uint8_t)v->valuedouble : 0;
    tmp.ack = cJSON_IsTrue(cJSON_GetObjectItem(o, "ack"));
    v = cJSON_GetObjectItem(o, "send");
    tmp.now = cJSON_IsString(v) && !strcmp(v->valuestring, "now");
    v = cJSON_GetObjectItem(o, "binding");
    if (cJSON_IsNumber(v)) { tmp.has_binding = true; tmp.binding = (uint32_t)v->valuedouble; }
    v = cJSON_GetObjectItem(o, "expires");
    tmp.expires = cJSON_IsNumber(v) ? (int64_t)v->valuedouble : wall_ms() + 600000;
    v = cJSON_GetObjectItem(o, "payload");
    int pl = cJSON_IsString(v) ? hex_decode(v->valuestring, tmp.payload, sizeof(tmp.payload)) : 0;
    if (pl < 0) err = "payload isn't hex, or is too long";
    else tmp.len = (size_t)pl;
    if (!find_net(net)) err = "this gateway doesn't serve that net";
    if (!roclink_is_node_addr(tmp.node)) err = "bad node";
    cJSON_Delete(o);
    if (!err) {
        for (size_t i = 0; i < MAX_DOWNLINKS; ++i)
            if (!g_dl[i].used) { d = &g_dl[i]; break; }
        if (!d) err = "the gateway's downlink queue is full";
    }
    if (err) {
        LOG_WARN("downlink %s refused: %s", tmp.id, err);
        dl_status(&tmp, "expired", err);
        return;
    }
    *d = tmp;
    d->used = true;
    d->order = g_dl_order++;
    LOG_INFO("downlink %s queued for %02X/%04X schema %u seq %u (%s)", d->id, net, d->node,
             d->schema, d->seq, d->now ? "now" : "window");
    if (d->now) dl_send_now(d);
}

static void dl_drain(void) {
    for (;;) {
        pthread_mutex_lock(&g_req_lock);
        dl_req_t *r = g_req_head;
        if (r) {
            g_req_head = r->next;
            if (!g_req_head) g_req_tail = NULL;
        }
        pthread_mutex_unlock(&g_req_lock);
        if (!r) return;
        dl_request(r->net, r->json);
        free(r->json);
        free(r);
    }
}

static void dl_expire(void) {
    const int64_t now = wall_ms();
    for (size_t i = 0; i < MAX_DOWNLINKS; ++i)
        if (g_dl[i].used && now >= g_dl[i].expires)
            dl_status(&g_dl[i], "expired", g_dl[i].offers ? "no ACK" : "the node didn't listen in time");
}

static void dl_acked(uint8_t net, uint16_t node, uint8_t seq) {
    for (size_t i = 0; i < MAX_DOWNLINKS; ++i) {
        downlink_t *d = &g_dl[i];
        if (d->used && d->net == net && d->node == node && d->seq == seq && d->offers)
            dl_status(d, "acked", NULL);
    }
}

/* ================================================================ uplinks */

static void publish_uplink(net_t *n, const roclink_rx_t *rx, const link_t *link) {
    static const char *const levels[] = {"none", "none", "auth", "enc"};
    char hex[2 * ROCLINK_PAYLOAD_MAX + 1], id[8];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "v", 1);
    snprintf(id, sizeof(id), "0x%04X", g_cfg.gateway_id);
    cJSON_AddStringToObject(o, "gateway", id);
    cJSON_AddNumberToObject(o, "time", (double)wall_ms());
    cJSON_AddNumberToObject(o, "net", n->id);
    snprintf(id, sizeof(id), "0x%04X", rx->hdr.src);
    cJSON_AddStringToObject(o, "src", id);
    cJSON_AddNumberToObject(o, "dst", rx->hdr.dst);
    cJSON_AddNumberToObject(o, "seq", rx->hdr.seq);
    cJSON_AddNumberToObject(o, "schema", rx->hdr.schema);
    cJSON_AddNumberToObject(o, "flags", rx->hdr.flags);
    cJSON_AddStringToObject(o, "sec", levels[rx->sec.level & 3]);
    if (rx->sec.level == ROCLINK_SEC_AUTH || rx->sec.level == ROCLINK_SEC_ENC) {
        cJSON_AddNumberToObject(o, "fcnt", rx->sec.fcnt);
        cJSON_AddNumberToObject(o, "key_gen", rx->sec.key_gen);
    }
    hex_encode(rx->payload, rx->payload_len, hex);
    cJSON_AddStringToObject(o, "payload", hex);
    cJSON *radio = cJSON_AddObjectToObject(o, "radio");
    cJSON_AddStringToObject(radio, "transport", link->kind == LINK_LORA ? "lora" : "udp");
    if (link->have_radio) {
        cJSON_AddNumberToObject(radio, "rssi", link->rssi);
        cJSON_AddNumberToObject(radio, "snr", link->snr);
    }
    publish(n->id, "up", o);
    cJSON_Delete(o);
    LOG_INFO("up %02X/%04X seq %u schema %u [%s] %zu bytes via %s", n->id, rx->hdr.src, rx->hdr.seq,
             rx->hdr.schema, levels[rx->sec.level & 3], rx->payload_len,
             link->kind == LINK_LORA ? "lora" : "udp");
}

/* Expensive opens (first contact, a ratchet step) are rationed per second. */
static bool budget_allows(net_t *n, int cost, uint32_t now) {
    if (cost <= CHEAP_OPEN) return true;
    if (now / 1000u != n->budget_second) {
        n->budget_second = now / 1000u;
        n->budget_used = 0;
    }
    return n->budget_used++ < COSTLY_PER_SEC;
}

/* The node is listening: ACK if asked, then its oldest downlink, in one burst,
 * each frame sealed and bound to the node frame that opened this reply. */
static void reply(net_t *n, const roclink_rx_t *rx, node_slot_t *slot, const link_t *link) {
    out_frame_t f[2];
    int count = 0;
    bool more;
    downlink_t *d = dl_for(n->id, rx->hdr.src, &more);
    const roclink_sec_info_t *opener = n->secured ? &rx->sec : NULL;
    const roclink_sec_peer_t *peer = n->secured && slot ? &slot->sec : NULL;
    if (n->secured && !peer) d = NULL; /* can't seal for an unknown node */

    if (rx->ack_requested) {
        f[count].len = roclink_server_build_ack(&n->srv, rx, d ? ROCLINK_FLAG_MORE : 0u, f[count].frame,
                                                sizeof(f[count].frame));
        if (f[count].len > 0) f[count].len = seal(n, f[count].frame, f[count].len, sizeof(f[count].frame), opener, peer);
        if (f[count].len < 0) LOG_WARN("cannot build the ACK: %s", roclink_status_str(f[count].len));
        count++;
    }
    if (d) {
        if (dl_build(n, d, more, opener, peer, &f[count]) < 0) {
            dl_status(d, "expired", roclink_status_str(f[count].len));
        } else {
            LOG_INFO("downlink %s to %04X: offer %u", d->id, d->node, d->offers + 1);
            count++;
            dl_sent(d);
        }
    }
    if (count) send_burst(link, f, count, true);
}

static void handle_frame(uint8_t *frame, size_t len, const link_t *link) {
    uint8_t raw[ROCLINK_FRAME_MAX_LEN];
    roclink_header_t h;
    const uint8_t *payload;
    size_t payload_len;
    roclink_rx_t rx;
    const uint32_t now = mono_ms();

    if (len > sizeof(raw)) { dropped(ROCLINK_E_TOO_LONG, frame, 64, link); return; }
    memcpy(raw, frame, len); /* for a drop report: open works in place */
    roclink_status_t st = roclink_decode(frame, len, &h, &payload, &payload_len);
    if (st != ROCLINK_OK) { dropped(st, raw, len, link); return; }
    net_t *n = find_net(h.net_id);
    if (!n) { dropped(ROCLINK_E_NET, raw, len, link); return; }
    if (!roclink_is_node_addr(h.src)) { dropped(ROCLINK_E_ADDR, raw, len, link); return; }

    node_slot_t *slot = find_node(n, h.src);
    if (n->secured) {
        /* A node seen before opens against its stored record. Anyone else is
         * tried against a scratch record, kept only if the frame verifies. */
        roclink_sec_peer_t trial;
        roclink_sec_peer_t *peer;
        roclink_sec_info_t info;
        if (slot) peer = &slot->sec;
        else {
            memset(&trial, 0, sizeof(trial));
            trial.policy = configured_policy(n->id, h.src);
            peer = &trial;
        }
        int cost = roclink_sec_open_cost(&n->sec, peer, frame, len);
        if (cost < 0) { dropped(cost, raw, len, link); return; }
        if (!budget_allows(n, cost, now)) {
            LOG_WARN("over the open budget (%d derivations): dropped unopened", cost);
            return;
        }
        int plain = roclink_sec_open(&n->sec, peer, frame, len, &info);
        if (plain < 0) { dropped(plain, raw, len, link); return; }
        if (!slot) {
            slot = find_node(n, 0);
            if (!slot) { dropped(ROCLINK_E_PEERS_FULL, raw, len, link); return; }
            slot->node = h.src;
            slot->sec = trial;
            LOG_INFO("net 0x%02X: new node %04X", n->id, h.src);
        }
        /* Persist before anything depends on it: a restart can't reopen a replay window. */
        if (info.level != ROCLINK_SEC_NONE && !info.retransmission) persist(n);
        roclink_server_handle_opened(&n->srv, frame, (size_t)plain, now, &info, &rx);
    } else {
        roclink_server_handle(&n->srv, frame, len, now, &rx);
        if (rx.kind != ROCLINK_RX_DROP && !slot) {
            slot = find_node(n, 0);
            if (slot) slot->node = h.src;
        }
    }

    if (rx.kind != ROCLINK_RX_DROP && slot) slot->link = *link;
    switch (rx.kind) {
        case ROCLINK_RX_DATA: publish_uplink(n, &rx, link); break;
        case ROCLINK_RX_CONTROL:
            if (rx.hdr.schema == ROCLINK_SCHEMA_CONTROL && rx.payload_len == 1 && rx.payload[0] == 0x01)
                publish_uplink(n, &rx, link); /* PING: published as schema 0, payload 01 */
            break;
        case ROCLINK_RX_DUPLICATE:
            LOG_DEBUG("%02X/%04X seq %u: retransmission, not published again", n->id, rx.hdr.src, rx.hdr.seq);
            break;
        case ROCLINK_RX_ACK: dl_acked(n->id, rx.hdr.src, rx.hdr.seq); break;
        case ROCLINK_RX_DROP: dropped(rx.status, raw, len, link); return;
    }
    if (rx.listening) reply(n, &rx, slot, link);
}

/* ======================================================= Semtech UDP (GWMP) */

static void semtech_rx(void) {
    uint8_t buf[65536];
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    ssize_t r = recvfrom(g_semtech_fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
    if (r < 4) return;
    const uint8_t ver = buf[0], ident = buf[3];
    uint8_t ack[4] = {ver, buf[1], buf[2], 0};
    if (ident == 0x02) { /* PULL_DATA: the forwarder's downstream address */
        ack[3] = 0x04;
        sendto(g_semtech_fd, ack, 4, 0, (struct sockaddr *)&from, fl);
        if (!g_fwd_known) LOG_INFO("packet forwarder connected (protocol %u)", ver);
        g_fwd_down = from;
        g_fwd_known = true;
        g_fwd_ver = ver;
        return;
    }
    if (ident == 0x05) { /* TX_ACK */
        buf[r] = 0;
        if (r > 12) {
            cJSON *o = cJSON_Parse((char *)buf + 12);
            const cJSON *a = o ? cJSON_GetObjectItem(o, "txpk_ack") : NULL;
            const cJSON *e = a ? cJSON_GetObjectItem(a, "error") : NULL;
            if (cJSON_IsString(e) && strcmp(e->valuestring, "NONE"))
                LOG_WARN("the packet forwarder refused a transmission: %s", e->valuestring);
            cJSON_Delete(o);
        }
        return;
    }
    if (ident != 0x00 || r < 12) return; /* PUSH_DATA only from here */
    ack[3] = 0x01;
    sendto(g_semtech_fd, ack, 4, 0, (struct sockaddr *)&from, fl);
    buf[r] = 0;
    cJSON *o = cJSON_Parse((char *)buf + 12);
    const cJSON *rxpk = o ? cJSON_GetObjectItem(o, "rxpk") : NULL;
    const cJSON *p;
    cJSON_ArrayForEach(p, rxpk) {
        const cJSON *stat = cJSON_GetObjectItem(p, "stat"), *modu = cJSON_GetObjectItem(p, "modu");
        const cJSON *data = cJSON_GetObjectItem(p, "data"), *tmst = cJSON_GetObjectItem(p, "tmst");
        const cJSON *rssi = cJSON_GetObjectItem(p, "rssi"), *lsnr = cJSON_GetObjectItem(p, "lsnr");
        if (!cJSON_IsNumber(stat) || stat->valueint != 1) continue; /* CRC bad or absent */
        if (!cJSON_IsString(modu) || strcmp(modu->valuestring, "LORA") || !cJSON_IsString(data)) continue;
        uint8_t frame[ROCLINK_FRAME_MAX_LEN];
        int n = b64_decode(data->valuestring, frame, sizeof(frame));
        if (n <= 0) continue;
        link_t link = {.kind = LINK_LORA};
        if (cJSON_IsNumber(tmst)) { link.tmst = (uint32_t)tmst->valuedouble; link.tmst_valid = true; }
        if (cJSON_IsNumber(rssi)) { link.rssi = (float)rssi->valuedouble; link.have_radio = true; }
        if (cJSON_IsNumber(lsnr)) link.snr = (float)lsnr->valuedouble;
        handle_frame(frame, (size_t)n, &link);
    }
    if (!rxpk && o && !cJSON_GetObjectItem(o, "stat")) LOG_DEBUG("PUSH_DATA without rxpk");
    cJSON_Delete(o);
}

static void udp_rx(void) {
    uint8_t frame[ROCLINK_FRAME_MAX_LEN + 1];
    link_t link = {.kind = LINK_UDP};
    socklen_t fl = sizeof(link.udp);
    ssize_t r = recvfrom(g_udp_fd, frame, sizeof(frame), 0, (struct sockaddr *)&link.udp, &fl);
    if (r <= 0) return;
    if ((size_t)r > ROCLINK_FRAME_MAX_LEN) { dropped(ROCLINK_E_TOO_LONG, frame, 64, &link); return; }
    handle_frame(frame, (size_t)r, &link);
}

static int udp_socket(const char *bind_addr, int port) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int one = 1;
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (fd < 0 || inet_pton(AF_INET, bind_addr, &a.sin_addr) != 1) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        LOG_ERR("cannot bind udp %s:%d: %s", bind_addr, port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* ============================================================== config file */

static int parse_net(char *v) {
    char *tok = strtok(v, " \t");
    size_t i;
    for (i = 0; i < MAX_NETS && g_nets[i].used; ++i) {}
    if (!tok || i == MAX_NETS) return -1;
    net_t *n = &g_nets[i];
    memset(n, 0, sizeof(*n));
    n->used = true;
    n->id = (uint8_t)strtoul(tok, NULL, 0);
    char *mode = strtok(NULL, " \t");
    if (!mode) return -1;
    roclink_server_config_t sc;
    roclink_server_config_default(&sc);
    sc.net_id = n->id;
    sc.gateway_id = g_cfg.gateway_id;
    uint32_t seed;
    getrandom(&seed, sizeof(seed), 0);
    if (roclink_server_init(&n->srv, &sc, n->peers, MAX_NODES, seed) != ROCLINK_OK) return -1;
    if (!strcmp(mode, "open")) {
        n->secured = false;
        LOG_INFO("net 0x%02X: open (unsecured frames only)", n->id);
        return 0;
    }
    if (strcmp(mode, "secured")) return -1;
    n->secured = true;
    uint8_t master[ROCLINK_KEY_LEN];
    bool have_master = false;
    for (char *opt; (opt = strtok(NULL, " \t"));) {
        if (!strncmp(opt, "master=", 7)) {
            have_master = hex_decode(opt + 7, master, sizeof(master)) == ROCLINK_KEY_LEN;
        } else if (!strncmp(opt, "master_file=", 12)) {
            FILE *f = fopen(opt + 12, "r");
            char line[80] = "";
            if (f) { if (!fgets(line, sizeof(line), f)) line[0] = 0; fclose(f); }
            have_master = hex_decode(trim(line), master, sizeof(master)) == ROCLINK_KEY_LEN;
            roclink_crypto_wipe(line, sizeof(line));
        }
    }
    if (!have_master) {
        LOG_ERR("net 0x%02X: secured needs master=HEX or master_file=PATH (32 hex digits)", n->id);
        return -1;
    }
    static const uint8_t demo[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    roclink_sec_config_t sec;
    roclink_sec_config_default(&sec);
    sec.net_id = n->id;
    sec.addr = g_cfg.gateway_id;
    if (roclink_sec_init_gateway(&n->sec, &sec, master, 0) != ROCLINK_OK) {
        LOG_ERR("net 0x%02X: security setup failed (crypto self-test?)", n->id);
        return -1;
    }
    LOG_INFO("net 0x%02X: secured, master mode, require-enc%s", n->id,
             memcmp(master, demo, 16) ? "" : ". The DEMO master key: never deploy it");
    roclink_crypto_wipe(master, sizeof(master));
    snprintf(n->state_path, sizeof(n->state_path), "%s/net-%02x.state", g_cfg.state_dir, n->id);
    return 0;
}

static int parse_policy(char *v, uint8_t policy) {
    unsigned net, node;
    if (g_policy_count == MAX_POLICIES || sscanf(v, "%x:%x", &net, &node) != 2) return -1;
    g_policies[g_policy_count++] = (policy_t){(uint8_t)net, (uint16_t)node, policy};
    return 0;
}

static int load_config(const char *path) {
    FILE *f = fopen(path, "r");
    char line[512];
    int lineno = 0;
    char *nets[MAX_NETS] = {0};
    size_t net_count = 0;
    if (!f) { LOG_ERR("cannot open %s: %s", path, strerror(errno)); return -1; }
    while (fgets(line, sizeof(line), f)) {
        ++lineno;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        char *k = trim(line);
        if (!*k) continue;
        if (!eq) { LOG_ERR("%s:%d: expected key = value", path, lineno); fclose(f); return -1; }
        *eq = 0;
        k = trim(line);
        char *v = trim(eq + 1);
#define STR(name) else if (!strcmp(k, #name)) snprintf(g_cfg.name, sizeof(g_cfg.name), "%s", v)
#define INT(name) else if (!strcmp(k, #name)) g_cfg.name = (int)strtol(v, NULL, 0)
        if (!strcmp(k, "gateway_id")) g_cfg.gateway_id = (uint16_t)strtoul(v, NULL, 0);
        STR(state_dir); STR(mqtt_host); INT(mqtt_port); STR(mqtt_user); STR(mqtt_client_id);
        STR(semtech_bind); INT(semtech_port); STR(udp_bind); INT(udp_port);
        INT(lora_sf); INT(lora_bw_khz); INT(lora_cr); INT(lora_power); INT(lora_preamble);
        INT(lora_reply_delay_ms);
        else if (!strcmp(k, "lora_freq_hz")) g_cfg.lora_freq_mhz = strtod(v, NULL) / 1e6;
        else if (!strcmp(k, "mqtt_password_file")) {
            FILE *pf = fopen(v, "r");
            if (!pf || !fgets(g_cfg.mqtt_password, sizeof(g_cfg.mqtt_password), pf)) {
                LOG_ERR("cannot read %s", v);
                if (pf) fclose(pf);
                fclose(f);
                return -1;
            }
            fclose(pf);
            memmove(g_cfg.mqtt_password, trim(g_cfg.mqtt_password), strlen(trim(g_cfg.mqtt_password)) + 1);
        } else if (!strcmp(k, "net")) {
            if (net_count == MAX_NETS) { LOG_ERR("%s:%d: at most %d nets", path, lineno, MAX_NETS); fclose(f); return -1; }
            nets[net_count++] = strdup(v); /* after gateway_id and state_dir are known */
        } else if (!strcmp(k, "allow_none") || !strcmp(k, "deny")) {
            if (parse_policy(v, !strcmp(k, "deny") ? ROCLINK_POLICY_DENY : ROCLINK_POLICY_ALLOW_NONE)) {
                LOG_ERR("%s:%d: expected NET:NODE in hex", path, lineno);
                fclose(f);
                return -1;
            }
        } else {
            LOG_ERR("%s:%d: unknown setting %s", path, lineno, k);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    if (!roclink_is_gateway_addr(g_cfg.gateway_id)) { LOG_ERR("gateway_id must be 0xFF00..0xFFFE"); return -1; }
    if (!net_count) { LOG_ERR("no net configured"); return -1; }
    for (size_t i = 0; i < net_count; ++i) {
        int r = parse_net(nets[i]);
        roclink_crypto_wipe(nets[i], strlen(nets[i]));
        free(nets[i]);
        if (r) { LOG_ERR("bad net line"); return -1; }
    }
    if (!*g_cfg.mqtt_client_id)
        snprintf(g_cfg.mqtt_client_id, sizeof(g_cfg.mqtt_client_id), "roclink-gw-%04x", g_cfg.gateway_id);
    return 0;
}

/* =================================================================== main */

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv) {
    const char *conf = "/etc/roclink-gw/gw.conf";
    for (int a = 1; a < argc; ++a) {
        if (!strcmp(argv[a], "-c") && a + 1 < argc) conf = argv[++a];
        else if (!strcmp(argv[a], "-v")) g_verbose = 1;
        else { fprintf(stderr, "usage: roclink-gw [-c CONFIG] [-v]\n"); return 2; }
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (load_config(conf)) return 1;
    for (size_t i = 0; i < MAX_NETS; ++i)
        if (g_nets[i].used && g_nets[i].secured) load_state(&g_nets[i]);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    if (pipe2(g_wake, O_NONBLOCK | O_CLOEXEC)) return 1;

    g_semtech_fd = udp_socket(g_cfg.semtech_bind, g_cfg.semtech_port);
    if (g_semtech_fd < 0) return 1;
    if (g_cfg.udp_port > 0) {
        g_udp_fd = udp_socket(g_cfg.udp_bind, g_cfg.udp_port);
        if (g_udp_fd < 0) return 1;
    }

    mosquitto_lib_init();
    g_mosq = mosquitto_new(g_cfg.mqtt_client_id, false, NULL); /* persistent session: queued downlink survives */
    if (!g_mosq) return 1;
    if (*g_cfg.mqtt_user) mosquitto_username_pw_set(g_mosq, g_cfg.mqtt_user, g_cfg.mqtt_password);
    mosquitto_connect_callback_set(g_mosq, on_connect);
    mosquitto_disconnect_callback_set(g_mosq, on_disconnect);
    mosquitto_message_callback_set(g_mosq, on_message);
    mosquitto_reconnect_delay_set(g_mosq, 1, 30, true);
    mosquitto_connect_async(g_mosq, g_cfg.mqtt_host, g_cfg.mqtt_port, 30);
    mosquitto_loop_start(g_mosq);

    LOG_INFO("gateway %04X: semtech udp %s:%d, wi-fi udp %s, lora %.3f MHz SF%d BW%d 4/%d",
             g_cfg.gateway_id, g_cfg.semtech_bind, g_cfg.semtech_port,
             g_cfg.udp_port > 0 ? "on" : "off", g_cfg.lora_freq_mhz, g_cfg.lora_sf,
             g_cfg.lora_bw_khz, g_cfg.lora_cr);
    g_report_window = mono_ms();

    while (!g_stop) {
        struct pollfd p[3] = {{g_semtech_fd, POLLIN, 0}, {g_wake[0], POLLIN, 0}, {g_udp_fd, POLLIN, 0}};
        int r = poll(p, g_udp_fd >= 0 ? 3 : 2, 1000);
        if (r < 0 && errno != EINTR) break;
        if (p[0].revents & POLLIN) semtech_rx();
        if (g_udp_fd >= 0 && (p[2].revents & POLLIN)) udp_rx();
        if (p[1].revents & POLLIN) {
            char junk[64];
            while (read(g_wake[0], junk, sizeof(junk)) > 0) {}
        }
        dl_drain();
        dl_expire();
        poll_reports(mono_ms());
    }
    LOG_INFO("stopping");
    flush_reports();
    mosquitto_disconnect(g_mosq);
    mosquitto_loop_stop(g_mosq, false);
    for (size_t i = 0; i < MAX_NETS; ++i)
        if (g_nets[i].used && g_nets[i].secured) roclink_sec_wipe(&g_nets[i].sec);
    mosquitto_destroy(g_mosq);
    mosquitto_lib_cleanup();
    return 0;
}
