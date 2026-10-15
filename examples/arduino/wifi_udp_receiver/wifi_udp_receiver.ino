/*
 *  roclink over Wi-Fi (UDP): receiver
 *
 *  The Arduino-ESP32 example "WiFiUDPClient" turned around: it listens on the
 *  UDP port and unwraps roclink envelopes. The Wi-Fi code is unchanged; lines
 *  marked "// roclink" are the roclink changes.
 *
 *  Set the same network name, password and port as wifi_udp_sender.
 */
#include <WiFi.h>
#include <NetworkUdp.h>

#include <roclink.h>  // roclink

#define ROCLINK_NET 0xF0        // roclink: an open test net: unsecured, anyone may join (PROTOCOL.md Annex B)
#define ROCLINK_GATEWAY 0xFF01  // roclink: this gateway's address (0xFF00..0xFFFE)

// WiFi network name and password:
const char *networkName = "your-ssid";
const char *networkPswd = "your-password";

const int udpPort = 3333;

void connectToWiFi(const char *ssid, const char *pwd);
void WiFiEvent(WiFiEvent_t event);

//Are we currently connected?
boolean connected = false;

//The udp library class
NetworkUDP udp;

roclink_server_t gateway;  // roclink
roclink_peer_t peers[16];  // roclink: per-node state (duplicates), up to 16 nodes

void setup() {
  // Initialize hardware serial:
  Serial.begin(115200);

  // roclink: set up the gateway side
  roclink_server_config_t cfg;
  roclink_server_config_default(&cfg);
  cfg.net_id = ROCLINK_NET;
  cfg.gateway_id = ROCLINK_GATEWAY;
  roclink_server_init(&gateway, &cfg, peers, 16, esp_random());

  //Connect to the WiFi network
  connectToWiFi(networkName, networkPswd);
}

void loop() {
  //only receive data when connected
  int size = connected ? udp.parsePacket() : 0;
  if (size > (int)ROCLINK_FRAME_MAX_LEN) {  // roclink: bigger than any frame this build accepts
    Serial.printf("Dropped a %d-byte packet from %s\n", size, udp.remoteIP().toString().c_str());
  } else if (size > 0) {  // roclink: never read part of a datagram, it would cut the frame short
    uint8_t frame[ROCLINK_FRAME_MAX_LEN];  // roclink: room for the largest frame
    int n = udp.read(frame, sizeof(frame));
    Serial.printf("Packet from %s, %d bytes\n", udp.remoteIP().toString().c_str(), n);

    // roclink: open the envelope
    roclink_rx_t rx;
    roclink_server_handle(&gateway, frame, n > 0 ? n : 0, millis(), &rx);
    if (rx.kind == ROCLINK_RX_DATA) {
      Serial.printf("  roclink: node %04X, schema %u, seq %u\n", rx.hdr.src, rx.hdr.schema, rx.hdr.seq);
      Serial.printf("  Message: %.*s\n", (int)rx.payload_len, (const char *)rx.payload);
    } else if (rx.kind == ROCLINK_RX_DUPLICATE) {
      Serial.println("  roclink: duplicate, ignored");
    } else {
      Serial.printf("  roclink: not for us (%s)\n", roclink_status_str(rx.status));
    }
  }
  delay(10);
}

void connectToWiFi(const char *ssid, const char *pwd) {
  Serial.println("Connecting to WiFi network: " + String(ssid));

  // delete old config
  WiFi.disconnect(true);
  //register event handler
  WiFi.onEvent(WiFiEvent);  // Will call WiFiEvent() from another thread.

  //Initiate connection
  WiFi.begin(ssid, pwd);

  Serial.println("Waiting for WIFI connection...");
}

// WARNING: WiFiEvent is called from a separate FreeRTOS task (thread)!
void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      //When connected set
      Serial.print("WiFi connected! IP address: ");
      Serial.println(WiFi.localIP());
      // listen on the port on every address, so broadcasts arrive too
      udp.begin(udpPort);
      connected = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("WiFi lost connection");
      connected = false;
      break;
    default: break;
  }
}
