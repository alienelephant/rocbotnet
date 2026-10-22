/*
    roclink over ESP-NOW: receiver

    The Arduino-ESP32 example "ESP_NOW_Broadcast_Slave", unwrapping roclink
    envelopes. Lines marked "// roclink" are the only changes; everything else
    is the stock example.

    Like the stock example, the first broadcast from a new sender only
    registers it; messages are printed from then on.
*/

#include "ESP32_NOW.h"
#include "WiFi.h"

#include <esp_mac.h>  // For the MAC2STR and MACSTR macros

#include <vector>

#include <roclink.h>  // roclink

/* Definitions */

#define ESPNOW_WIFI_CHANNEL 6

#define ROCLINK_NET 0xF0        // roclink: an open test net: unsecured, anyone may join (PROTOCOL.md Annex B)
#define ROCLINK_GATEWAY 0xFF01  // roclink: this gateway's address (0xFF00..0xFFFE)

/* Global Variables */

roclink_server_t gateway;   // roclink
roclink_peer_t peers[16];   // roclink: per-node state (duplicates), up to 16 nodes

/* Classes */

// Creating a new class that inherits from the ESP_NOW_Peer class is required.

class ESP_NOW_Peer_Class : public ESP_NOW_Peer {
public:
  // Constructor of the class
  ESP_NOW_Peer_Class(const uint8_t *mac_addr, uint8_t channel, wifi_interface_t iface, const uint8_t *lmk) : ESP_NOW_Peer(mac_addr, channel, iface, lmk) {}

  // Destructor of the class
  ~ESP_NOW_Peer_Class() {}

  // Function to register the master peer
  bool add_peer() {
    if (!add()) {
      log_e("Failed to register the broadcast peer");
      return false;
    }
    return true;
  }

  // Function to print the received messages from the master
  void onReceive(const uint8_t *data, size_t len, bool broadcast) {
    Serial.printf("Received a message from master " MACSTR " (%s)\n", MAC2STR(addr()), broadcast ? "broadcast" : "unicast");

    // roclink: open the envelope
    roclink_rx_t rx;
    roclink_server_handle(&gateway, data, len, millis(), &rx);
    if (rx.kind == ROCLINK_RX_DATA) {
      Serial.printf("  roclink: node %04X, schema %u, seq %u\n", rx.hdr.src, rx.hdr.schema, rx.hdr.seq);
      Serial.printf("  Message: %.*s\n", (int)rx.payload_len, (const char *)rx.payload);
    } else if (rx.kind == ROCLINK_RX_DUPLICATE) {
      Serial.println("  roclink: duplicate, ignored");
    } else {
      Serial.printf("  roclink: not for us (%s)\n", roclink_status_str(rx.status));
    }
  }
};

// List of all the masters. It will be populated when a new master is registered
// Note: Using pointers instead of objects to prevent dangling pointers when the vector reallocates
std::vector<ESP_NOW_Peer_Class *> masters;

/* Callbacks */

// Callback called when an unknown peer sends a message
void register_new_master(const esp_now_recv_info_t *info, const uint8_t *data, int len, void *arg) {
  if (memcmp(info->des_addr, ESP_NOW.BROADCAST_ADDR, 6) == 0) {
    Serial.printf("Unknown peer " MACSTR " sent a broadcast message\n", MAC2STR(info->src_addr));
    Serial.println("Registering the peer as a master");

    ESP_NOW_Peer_Class *new_master = new ESP_NOW_Peer_Class(info->src_addr, ESPNOW_WIFI_CHANNEL, WIFI_IF_STA, nullptr);
    if (!new_master->add_peer()) {
      Serial.println("Failed to register the new master");
      delete new_master;
      return;
    }
    masters.push_back(new_master);
    Serial.printf("Successfully registered master " MACSTR " (total masters: %zu)\n", MAC2STR(new_master->addr()), masters.size());
  } else {
    // The slave will only receive broadcast messages
    log_v("Received a unicast message from " MACSTR, MAC2STR(info->src_addr));
    log_v("Igorning the message");
  }
}

/* Main */

void setup() {
  Serial.begin(115200);

  // Initialize the Wi-Fi module
  WiFi.mode(WIFI_STA);
  WiFi.setChannel(ESPNOW_WIFI_CHANNEL);
  while (!WiFi.STA.started()) {
    delay(100);
  }

  Serial.println("ESP-NOW Example - Broadcast Slave (roclink)");
  Serial.println("Wi-Fi parameters:");
  Serial.println("  Mode: STA");
  Serial.println("  MAC Address: " + WiFi.macAddress());
  Serial.printf("  Channel: %d\n", ESPNOW_WIFI_CHANNEL);

  // roclink: set up the gateway side
  roclink_server_config_t cfg;
  roclink_server_config_default(&cfg);
  cfg.net_id = ROCLINK_NET;
  cfg.gateway_id = ROCLINK_GATEWAY;
  roclink_server_init(&gateway, &cfg, peers, 16, esp_random());

  // Initialize the ESP-NOW protocol
  if (!ESP_NOW.begin()) {
    Serial.println("Failed to initialize ESP-NOW");
    Serial.println("Reeboting in 5 seconds...");
    delay(5000);
    ESP.restart();
  }

  Serial.printf("ESP-NOW version: %d, max data length: %d\n", ESP_NOW.getVersion(), ESP_NOW.getMaxDataLen());

  // Register the new peer callback
  ESP_NOW.onNewPeer(register_new_master, nullptr);

  Serial.println("Setup complete. Waiting for a master to broadcast a message...");
}

void loop() {
  // Print debug information every 10 seconds
  static unsigned long last_debug = 0;
  if (millis() - last_debug > 10000) {
    last_debug = millis();
    Serial.printf("Registered masters: %zu\n", masters.size());
    for (size_t i = 0; i < masters.size(); i++) {
      if (masters[i]) {
        Serial.printf("  Master %zu: " MACSTR "\n", i, MAC2STR(masters[i]->addr()));
      }
    }
  }

  delay(100);
}
