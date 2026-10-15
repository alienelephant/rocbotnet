/*
  roclink over LoRa, secured: receiver (gateway 0xFF01)

  A Heltec WiFi LoRa 32 V2 at 915 MHz that accepts encrypted frames from
  nodes 0x1234 (lora_secure_sender) and 0x1235 (a second node, for example
  an Uno with roclink's roclink-crypto extension), and answers with sealed ACKs.

  How a secured gateway works (PROTOCOL.md 10.7, SECURITY.md):
    - Enrolled mode: it holds each node's chain key, never the master key
      (SECURITY.md R3), so a stolen gateway reveals no earlier traffic.
      Keys: secrets_demo.h, unless you add a secrets.h with your own.
    - Each node's peer record lives in NVS, and is stored every time a new
      frame from it is accepted, so a restart can't reopen a replay window.
    - Every frame is opened before the gateway logic sees it. Its cost is
      checked first: a forged frame can't make the gateway do much work.
    - Each ACK is sealed and bound to the node frame it answers.
    - Every dropped frame is printed with its reason and RSSI. Dropped frames
      are where forgery, replays and cloned nodes show up (PROTOCOL.md §9
      rule 8); a networked gateway would pass them on to the hub.

  For a deployment (SECURITY.md R1): flash encryption, secure boot and an
  encrypted NVS partition.
*/
#include <Preferences.h>
#include <RadioLib.h>

#include <roclink.h>
#include <roclink_secure.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets_demo.h"
#endif

#define ROCLINK_NET 0x42
#define ROCLINK_GATEWAY 0xFF01  // this gateway's address (0xFF00..0xFFFE)

// The nodes this gateway accepts, and their provisioned keys.
struct enrolled_t {
  uint16_t node;
  const uint8_t *chain_key;
};
static const enrolled_t NODES[] = {
    {0x1234, NODE_1234_CHAIN_KEY},
    {0x1235, NODE_1235_CHAIN_KEY},
};
#define NODE_COUNT (sizeof(NODES) / sizeof(NODES[0]))

// Heltec WiFi LoRa 32 V2 (SX1276): NSS 18, DIO0 26, RESET 14, DIO1 35
SX1276 radio = new Module(18, 26, 14, 35);

Preferences store;
roclink_server_t gateway;
roclink_peer_t peers[16];                    // duplicate state
roclink_sec_t sec;                           // security context: no master key
roclink_sec_peer_t sec_peers[NODE_COUNT];    // one record per enrolled node
roclink_fcnt_t ctr;                          // the gateway's own frame counter

static void halt(const char *what) {
  Serial.println(what);
  while (true) delay(1000);
}

static void peer_name(char name[8], uint16_t node) { snprintf(name, 8, "p%04x", node); }

static void store_peer(size_t i) {
  uint8_t rec[ROCLINK_SEC_PEER_RECORD_LEN];
  char name[8];
  peer_name(name, NODES[i].node);
  roclink_sec_peer_save(&sec_peers[i], rec);
  if (store.putBytes(name, rec, sizeof(rec)) != sizeof(rec)) halt("cannot store a peer record");
  roclink_crypto_wipe(rec, sizeof(rec));
}

static int peer_index(uint16_t node) {
  for (size_t i = 0; i < NODE_COUNT; ++i) {
    if (NODES[i].node == node) return (int)i;
  }
  return -1;
}

// A dropped frame: print why. Everything but the reason comes from the
// frame itself, so a claimed address is only a claim.
static void dropped(int status, size_t n) {
  Serial.printf("[roclink] dropped a %u-byte frame: %s (RSSI %.0f dBm)\n", (unsigned)n,
                roclink_status_str(status), radio.getRSSI());
}

// Seals a gateway frame, storing the counter reservation first when asked.
static int seal(uint8_t *frame, int len, size_t cap, const roclink_sec_info_t *opener,
                const roclink_sec_peer_t *peer) {
  for (;;) {
    int n = roclink_sec_seal(&sec, &ctr, opener, peer, frame, (size_t)len, frame, cap);
    if (n != ROCLINK_E_PERSIST) return n;
    uint32_t v = roclink_fcnt_to_persist(&ctr);
    if (store.putULong("fcnt", v) != sizeof(uint32_t)) halt("cannot store the counter");
    roclink_fcnt_persisted(&ctr, v);
  }
}

void setup() {
  Serial.begin(115200);

  ConfigLoRa_t config;
  config.frequency = 915;
  if (radio.begin(config) != RADIOLIB_ERR_NONE) halt("radio failed");

  roclink_server_config_t cfg;
  roclink_server_config_default(&cfg);
  cfg.net_id = ROCLINK_NET;
  cfg.gateway_id = ROCLINK_GATEWAY;
  roclink_server_init(&gateway, &cfg, peers, 16, esp_random());

  // Security: enrolled mode (no master key), require-enc with 8-byte tags.
  roclink_sec_config_t sc;
  roclink_sec_config_default(&sc);
  sc.net_id = ROCLINK_NET;
  sc.addr = ROCLINK_GATEWAY;
  if (roclink_sec_init_gateway(&sec, &sc, NULL, 0) != ROCLINK_OK) halt("roclink security failed");

  // Each node's record from NVS, or its provisioned key on first boot.
  store.begin("roclink-gw", false);
  for (size_t i = 0; i < NODE_COUNT; ++i) {
    uint8_t rec[ROCLINK_SEC_PEER_RECORD_LEN];
    char name[8];
    peer_name(name, NODES[i].node);
    if (store.getBytes(name, rec, sizeof(rec)) != sizeof(rec) ||
        roclink_sec_peer_load(&sec_peers[i], rec) != ROCLINK_OK) {
      memset(&sec_peers[i], 0, sizeof(sec_peers[i]));
      roclink_sec_peer_enroll(&sec_peers[i], NODES[i].chain_key, 0, 0);
      store_peer(i);
    }
    roclink_crypto_wipe(rec, sizeof(rec));
  }
  roclink_fcnt_init(&ctr, store.getULong("fcnt", 0), 0);
  // Skip well past any value sent since this NVS might have been backed up
  // and restored (PROTOCOL.md 10.5); the next frame stores the new position.
  if (roclink_fcnt_skip(&ctr, ROCLINK_FCNT_RESTART_SKIP) != ROCLINK_OK) halt("frame counter exhausted: re-key");

  Serial.printf("[roclink] gateway %04X ready, %u nodes enrolled\n", ROCLINK_GATEWAY,
                (unsigned)NODE_COUNT);
}

void loop() {
  uint8_t frame[256];
  if (radio.receive(frame, 0, 1000) != RADIOLIB_ERR_NONE) return;  // 0 = any length
  size_t n = radio.getPacketLength();

  // Which node does it claim to be from? Unknown nodes get a blank record,
  // which roclink_sec_open() refuses (not enrolled) before any cryptography.
  roclink_header_t h;
  const uint8_t *payload;
  size_t payload_len;
  roclink_status_t st = roclink_decode(frame, n, &h, &payload, &payload_len);
  if (st != ROCLINK_OK) {
    dropped(st, n);
    return;
  }
  const int idx = peer_index(h.src);
  roclink_sec_peer_t blank;
  memset(&blank, 0, sizeof(blank));
  roclink_sec_peer_t *peer = idx >= 0 ? &sec_peers[idx] : &blank;

  // Refuse expensive opens: in enrolled mode, the cost is only a ratchet step.
  int cost = roclink_sec_open_cost(&sec, peer, frame, n);
  if (cost < 0) {
    dropped(cost, n);
    return;
  }
  roclink_sec_info_t info;
  int plain = roclink_sec_open(&sec, peer, frame, n, &info);
  if (plain < 0) {
    dropped(plain, n);
    return;
  }
  if (idx >= 0 && !info.retransmission) store_peer((size_t)idx);  // before anything else

  roclink_rx_t rx;
  roclink_server_handle_opened(&gateway, frame, (size_t)plain, millis(), &info, &rx);
  if (rx.kind == ROCLINK_RX_DATA) {
    Serial.printf("[roclink] node %04X, schema %u, seq %u, fcnt %lu, RSSI %.0f dBm\n", rx.hdr.src,
                  rx.hdr.schema, rx.hdr.seq, (unsigned long)info.fcnt, radio.getRSSI());
    Serial.printf("[roclink] data (%u bytes):", (unsigned)rx.payload_len);
    for (size_t i = 0; i < rx.payload_len; ++i) Serial.printf(" %02X", rx.payload[i]);
    Serial.println();
  } else if (rx.kind == ROCLINK_RX_DUPLICATE) {
    Serial.println(F("[roclink] retransmission: ACK again, not delivered again"));
  } else if (rx.kind == ROCLINK_RX_DROP) {
    dropped(rx.status, n);
  }

  if (rx.ack_requested) {  // sealed and bound to the frame it answers
    uint8_t ack[ROCLINK_HEADER_LEN + ROCLINK_SEC_OVERHEAD(8)];
    int an = roclink_server_build_ack(&gateway, &rx, 0, ack, sizeof(ack));
    if (an > 0) an = seal(ack, an, sizeof(ack), &rx.sec, peer);
    if (an > 0) radio.transmit(ack, (size_t)an);
  }
}
