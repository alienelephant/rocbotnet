/*
    roclink over ESP-NOW: sender

    The Arduino-ESP32 example "ESP_NOW_Broadcast_Master", with its message
    wrapped in a roclink envelope. Lines marked "// roclink" are the only
    changes; everything else is the stock example.

    Pair it with espnow_receiver. Both must use the same ESPNOW_WIFI_CHANNEL.
*/

#include "ESP32_NOW.h"
#include "WiFi.h"

#include <esp_mac.h>  // For the MAC2STR and MACSTR macros

#include <roclink.h>  // roclink

/* Definitions */

#define ESPNOW_WIFI_CHANNEL 6

#define ROCLINK_NET 0xF0   // roclink: an open test net: unsecured, anyone may join (PROTOCOL.md Annex B)
#define ROCLINK_NODE 0x1234  // roclink: this node's address; give each node its own
#define SCHEMA_HELLO 1       // roclink: what the payload is (your choice, 1..255)

/* Classes */

// Creating a new class that inherits from the ESP_NOW_Peer class is required.

class ESP_NOW_Broadcast_Peer : public ESP_NOW_Peer {
public:
  // Constructor of the class using the broadcast address
  ESP_NOW_Broadcast_Peer(uint8_t channel, wifi_interface_t iface, const uint8_t *lmk) : ESP_NOW_Peer(ESP_NOW.BROADCAST_ADDR, channel, iface, lmk) {}

  // Destructor of the class
  ~ESP_NOW_Broadcast_Peer() {
    remove();
  }

  // Function to properly initialize the ESP-NOW and register the broadcast peer
  bool begin() {
    if (!ESP_NOW.begin() || !add()) {
      log_e("Failed to initialize ESP-NOW or register the broadcast peer");
      return false;
    }
    return true;
  }

  // Function to send a message to all devices within the network
  bool send_message(const uint8_t *data, size_t len) {
    if (!send(data, len)) {
      log_e("Failed to broadcast message");
      return false;
    }
    return true;
  }
};

/* Global Variables */

uint32_t msg_count = 0;
roclink_client_t node;  // roclink

// Create a broadcast peer object
ESP_NOW_Broadcast_Peer broadcast_peer(ESPNOW_WIFI_CHANNEL, WIFI_IF_STA, nullptr);

/* Main */

void setup() {
  Serial.begin(115200);

  // Initialize the Wi-Fi module
  WiFi.mode(WIFI_STA);
  WiFi.setChannel(ESPNOW_WIFI_CHANNEL);
  while (!WiFi.STA.started()) {
    delay(100);
  }

  Serial.println("ESP-NOW Example - Broadcast Master (roclink)");
  Serial.println("Wi-Fi parameters:");
  Serial.println("  Mode: STA");
  Serial.println("  MAC Address: " + WiFi.macAddress());
  Serial.printf("  Channel: %d\n", ESPNOW_WIFI_CHANNEL);

  // Register the broadcast peer
  if (!broadcast_peer.begin()) {
    Serial.println("Failed to initialize broadcast peer");
    Serial.println("Reebooting in 5 seconds...");
    delay(5000);
    ESP.restart();
  }

  roclink_client_config_t cfg = {ROCLINK_NET, ROCLINK_NODE, ROCLINK_ADDR_ANY_GATEWAY};  // roclink
  roclink_client_init(&node, &cfg, (uint8_t)esp_random());                              // roclink

  Serial.printf("ESP-NOW version: %d, max data length: %d\n", ESP_NOW.getVersion(), ESP_NOW.getMaxDataLen());

  Serial.println("Setup complete. Broadcasting messages every 5 seconds.");
}

void loop() {
  // Broadcast a message to all devices within the network
  char data[32];
  snprintf(data, sizeof(data), "Hello, World! #%lu", msg_count++);

  // roclink: wrap the message in an envelope (9-byte header + your bytes)
  uint8_t frame[ROCLINK_FRAME_MAX_LEN];
  int len = roclink_client_build_data(&node, SCHEMA_HELLO, (const uint8_t *)data, strlen(data),
                                      ROCLINK_SEND_DEFAULT, frame, sizeof(frame));

  Serial.printf("Broadcasting message: %s (%d-byte roclink frame)\n", data, len);

  if (len < 0 || !broadcast_peer.send_message(frame, len)) {  // roclink: send the frame
    Serial.println("Failed to broadcast message");
  }

  delay(5000);
}
