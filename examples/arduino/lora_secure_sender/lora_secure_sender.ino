/*
  roclink over LoRa, secured: sender (node 0x1234)

  A Heltec WiFi LoRa 32 V2 at 915 MHz sends an encrypted, authenticated
  message every 10 seconds and asks for an ACK, retrying the same bytes if
  none comes. Pair it with lora_secure_receiver. (lora_sender is the same
  thing without security, on the open test net.)

  What security adds to lora_sender, all marked "// roclink sec":
    - a provisioned key: the demo key from secrets_demo.h, unless you add a
      secrets.h with your own (python3 tools/provision.py --master ... 1234);
    - persistence in NVS (Preferences): the frame-counter reservation, and the
      chain key with its epoch, each stored before anything that needs it is sent;
    - roclink_sec_seal() on the way out, roclink_sec_open() on the way in;
    - a daily ratchet, so a stolen board reveals no earlier day's traffic.

  For a deployment (SECURITY.md R1): enable flash encryption and secure boot,
  and keep the NVS partition encrypted. Without that, the key in NVS can be
  read from a stolen board.
*/
#include <Preferences.h>
#include <RadioLib.h>

#include <roclink.h>
#include <roclink_secure.h>  // roclink sec

#if __has_include("secrets.h")  // roclink sec: your key, git-ignored
#include "secrets.h"
#else
#include "secrets_demo.h"  // roclink sec: the published demo key, for development only
#endif

#define ROCLINK_NET 0x42     // the same on every device of the network
#define ROCLINK_NODE 0x1234  // this node's address; matches its key
#define SCHEMA_HELLO 1         // what the payload is (your choice, 1..255)
#define REPORT_MS 10000UL
#define ACK_WAIT_MS 1500     // listen this long for the gateway's ACK
#define RETRIES 2
#define EPOCH_MS (24UL * 60UL * 60UL * 1000UL)  // roclink sec: ratchet daily (SECURITY.md R5)

// Heltec WiFi LoRa 32 V2 (SX1276): NSS 18, DIO0 26, RESET 14, DIO1 35
SX1276 radio = new Module(18, 26, 14, 35);

Preferences store;     // roclink sec: NVS
roclink_client_t node;
roclink_sec_t sec;     // roclink sec
roclink_fcnt_t ctr;    // roclink sec: this node's frame counter
unsigned long last_report = 0, last_epoch = 0;
int count = 0;

static void halt(const char *what) {
  Serial.println(what);
  while (true) delay(1000);
}

// roclink sec: the key and its epoch, as one NVS entry (NVS replaces an entry
// atomically, and overwriting it erases the old key: SECURITY.md R4).
static void store_key(const uint8_t key[16], uint32_t epoch) {
  uint8_t rec[20];
  memcpy(rec, &epoch, 4);
  memcpy(rec + 4, key, 16);
  if (store.putBytes("key", rec, sizeof(rec)) != sizeof(rec)) halt("cannot store the key");
  roclink_crypto_wipe(rec, sizeof(rec));
}

// roclink sec: seal, storing first whatever a power cut must not lose.
static int seal(uint8_t *frame, int len, size_t cap) {
  for (;;) {
    int n = roclink_sec_seal(&sec, &ctr, NULL, NULL, frame, (size_t)len, frame, cap);
    if (n == ROCLINK_E_PERSIST) {
      uint32_t v = roclink_fcnt_to_persist(&ctr);
      if (store.putULong("fcnt", v) != sizeof(uint32_t)) halt("cannot store the counter");
      roclink_fcnt_persisted(&ctr, v);
    } else if (n == ROCLINK_E_PERSIST_KEY) {
      uint8_t key[16];
      uint32_t epoch = 0;
      roclink_sec_key_pending(&sec, key, &epoch);
      store_key(key, epoch);
      roclink_crypto_wipe(key, sizeof(key));
      roclink_sec_key_persisted(&sec, epoch);
      Serial.printf("[roclink] ratchet: epoch %lu, old key erased\n", (unsigned long)epoch);
    } else {
      return n;
    }
  }
}

void setup() {
  Serial.begin(115200);

  ConfigLoRa_t config;
  config.frequency = 915;
  if (radio.begin(config) != RADIOLIB_ERR_NONE) halt("radio failed");

  // roclink sec: restore the key and counter, or start from the provisioned key.
  store.begin("roclink", false);
  uint8_t rec[20];
  uint32_t epoch = 0;
  if (store.getBytes("key", rec, sizeof(rec)) != sizeof(rec)) {
    store_key(NODE_1234_CHAIN_KEY, 0);  // first boot
    memcpy(rec + 4, NODE_1234_CHAIN_KEY, 16);
  } else {
    memcpy(&epoch, rec, 4);
  }
  roclink_sec_config_t sc;
  roclink_sec_config_default(&sc);  // encrypted, 8-byte tags
  sc.net_id = ROCLINK_NET;
  sc.addr = ROCLINK_NODE;
  if (roclink_sec_init_node_at(&sec, &sc, rec + 4, 0, epoch) != ROCLINK_OK) halt("roclink security failed");
  roclink_crypto_wipe(rec, sizeof(rec));
  roclink_fcnt_init(&ctr, store.getULong("fcnt", 0), 0);  // continue from the reservation

  roclink_client_config_t cfg = {ROCLINK_NET, ROCLINK_NODE, ROCLINK_ADDR_ANY_GATEWAY};
  roclink_client_init(&node, &cfg, (uint8_t)esp_random());
  last_epoch = millis();
  Serial.printf("[roclink] node %04X ready, epoch %lu\n", ROCLINK_NODE, (unsigned long)epoch);
}

// Sends one secured message and waits for its ACK, retrying the same bytes.
static void report() {
  uint8_t frame[ROCLINK_FRAME_MAX_LEN];
  String str = "Hello World! #" + String(count++);
  int len = roclink_client_build_data(&node, SCHEMA_HELLO, (const uint8_t *)str.c_str(), str.length(),
                                      ROCLINK_SEND_ACK, frame, sizeof(frame));
  if (len > 0) len = seal(frame, len, sizeof(frame));  // roclink sec
  if (len < 0) {
    Serial.printf("[roclink] %s\n", roclink_status_str(len));
    return;
  }

  for (int attempt = 0; attempt <= RETRIES && roclink_client_awaiting_ack(&node); ++attempt) {
    Serial.printf("[SX1276] sending %d-byte sealed frame, seq %u%s\n", len, frame[7],
                  attempt ? " (retry)" : "");
    radio.transmit(frame, len);  // a retry is the identical sealed bytes
    uint8_t in[ROCLINK_FRAME_MAX_LEN];
    if (radio.receive(in, 0, ACK_WAIT_MS) == RADIOLIB_ERR_NONE) {
      roclink_sec_info_t info;
      int n = roclink_sec_open(&sec, NULL, in, radio.getPacketLength(), &info);  // roclink sec
      if (n < 0) {
        Serial.printf("[roclink] refused a frame: %s\n", roclink_status_str(n));
        continue;
      }
      roclink_rx_t rx;
      roclink_client_handle(&node, in, (size_t)n, &rx);
      if (rx.kind == ROCLINK_RX_ACK) Serial.println(F("[roclink] ACKed"));
    }
  }
  roclink_sec_close_window(&sec);  // roclink sec: a late reply is refused from here on
  if (roclink_client_awaiting_ack(&node)) {
    Serial.println(F("[roclink] no ACK, giving up on this one"));
    roclink_client_cancel_ack(&node);
  }
}

void loop() {
  // roclink sec: once a day, move to the next epoch; the next frame ratchets the key.
  // (This clock restarts with the board; a node that reboots daily needs real time.)
  if (millis() - last_epoch >= EPOCH_MS) {
    roclink_fcnt_next_epoch(&ctr);
    last_epoch += EPOCH_MS;
  }
  if (last_report == 0 || millis() - last_report >= REPORT_MS) {
    last_report = millis();
    report();
  }
}
