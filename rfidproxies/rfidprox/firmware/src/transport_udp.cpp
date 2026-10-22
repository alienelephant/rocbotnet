// Wi-Fi UDP to the rfgateway (DESIGN.md section 6.4): the roclink-nodes access
// point, roclink-gw on GATEWAY_IP:GATEWAY_PORT.
#if !RFIDPROX_LORA
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include "config.h"
#include "transport.h"

namespace transport {

static WiFiUDP udp;
static bool udp_open = false;
static IPAddress gateway(GATEWAY_IP);

bool begin() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[link] joining Wi-Fi %s\n", WIFI_SSID);
    return true;
}

bool ready() { return WiFi.status() == WL_CONNECTED; }

void loop() {
    if (ready() && !udp_open) {
        udp_open = udp.begin(GATEWAY_PORT) == 1;  // fixed port: the gateway answers here
        Serial.printf("[link] Wi-Fi up: %s, gateway %s:%d\n", WiFi.localIP().toString().c_str(),
                      gateway.toString().c_str(), GATEWAY_PORT);
    } else if (!ready() && udp_open) {
        udp.stop();
        udp_open = false;
        Serial.println("[link] Wi-Fi down");
    }
}

bool send(const uint8_t *frame, size_t len) {
    if (!udp_open) return false;
#ifdef PIN_TX_LED
    digitalWrite(PIN_TX_LED, HIGH);
#endif
    udp.beginPacket(gateway, GATEWAY_PORT);
    udp.write(frame, len);
    bool ok = udp.endPacket() == 1;
#ifdef PIN_TX_LED
    digitalWrite(PIN_TX_LED, LOW);
#endif
    return ok;
}

int receive(uint8_t *buf, size_t cap) {
    if (!udp_open || udp.parsePacket() <= 0) return 0;
    int n = udp.read(buf, cap);
    return n > 0 ? n : 0;
}

const char *describe() {
    static char s[32];
    if (!ready()) return "Wi-Fi: joining";
    snprintf(s, sizeof(s), "Wi-Fi %s", WiFi.localIP().toString().c_str());
    return s;
}

uint32_t ack_wait_ms() { return ACK_WAIT_MS; }

}  // namespace transport
#endif
