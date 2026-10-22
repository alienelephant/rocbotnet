/*
 *  roclink over Wi-Fi (UDP): sender
 *
 *  The Arduino-ESP32 example "WiFiUDPClient", sending its message inside a
 *  roclink envelope. Lines marked "// roclink" are the only changes;
 *  everything else is the stock example.
 *
 *  Set your network name and password, and the broadcast address of your
 *  network (or the receiver's IP). Pair it with wifi_udp_receiver.
 */
#include <WiFi.h>
#include <NetworkUdp.h>

#include <roclink.h>  // roclink

#define ROCLINK_NET 0xF0     // roclink: an open test net: unsecured, anyone may join (PROTOCOL.md Annex B)
#define ROCLINK_NODE 0x1234  // roclink: this node's address; give each node its own
#define SCHEMA_HELLO 1         // roclink: what the payload is (your choice, 1..255)

// WiFi network name and password:
const char *networkName = "jv_testing";
const char *networkPswd = "jessieisagoodgirl";

//IP address to send UDP data to:
// either use the ip address of the server or
// a network broadcast address
const char *udpAddress = "192.168.8.255";
const int udpPort = 3333;

const int IR_LED_PIN = 1;
const int IR_DIODE_PIN = 2; 
const int LED_BULTIN = 21;
const char* TRIGGER = "TRIGGER";

void connectToWiFi(const char *ssid, const char *pwd);
void WiFiEvent(WiFiEvent_t event);

//Are we currently connected?
boolean connected = false;
bool triggered = false;

//The udp library class
NetworkUDP udp;

roclink_client_t node;  // roclink

void setup() {
  // Initialize hardware serial:
  Serial.begin(115200);


  // setup leds
  pinMode(LED_BULTIN, OUTPUT);
  digitalWrite(LED_BULTIN, LOW);

  roclink_client_config_t cfg = {ROCLINK_NET, ROCLINK_NODE, ROCLINK_ADDR_ANY_GATEWAY};  // roclink
  roclink_client_init(&node, &cfg, (uint8_t)esp_random());                              // roclink

  //Connect to the WiFi network
  connectToWiFi(networkName, networkPswd);
}

void loop() {
  //only send data when connected
  if (connected) {
    char data[32];
    snprintf(data, sizeof(data), "Seconds since boot: %lu", millis() / 1000);

    // roclink: wrap the message in an envelope (9-byte header + your bytes)
    uint8_t frame[ROCLINK_FRAME_MAX_LEN];
    int len = roclink_client_build_data(&node, SCHEMA_HELLO, (const uint8_t *)data, strlen(data),
                                        ROCLINK_SEND_DEFAULT, frame, sizeof(frame));

    if (len < 0) {                                              // roclink: no frame was built,
      Serial.printf("roclink: %s\n", roclink_status_str(len));  // roclink: so send nothing
    } else {
      //Send a packet
      udp.beginPacket(udpAddress, udpPort);
      udp.write(frame, len);  // roclink: send the frame instead of printf
      udp.endPacket();
      Serial.printf("Sent: %s (%d-byte roclink frame)\n", data, len);
    }
  }
  //Wait for 1 second
  delay(1000);
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
      //initializes the UDP state
      //This initializes the transfer buffer
      udp.begin(WiFi.localIP(), udpPort);
      connected = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("WiFi lost connection");
      connected = false;
      break;
    default: break;
  }
}
