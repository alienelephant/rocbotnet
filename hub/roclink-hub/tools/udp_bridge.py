"""Bench bridge: roclink frames over UDP -> MQTT, for testing the hub.

Listens where examples/arduino/wifi_udp_sender sends (UDP port 3333) and
publishes each datagram, unchanged, to roclink/in/<net>/<gateway>/up. It acts as
a gateway that only moves bytes: no ACKs, no downlink, no security. Real
gateways should check frames and publish JSON forward records instead.

    python tools/udp_bridge.py --broker localhost --gateway 0xFF01
"""

import argparse
import socket

import paho.mqtt.client as mqtt


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--listen", default="0.0.0.0")
    parser.add_argument("--udp-port", type=int, default=3333)
    parser.add_argument("--broker", default="localhost")
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--gateway", type=lambda s: int(s, 0), default=0xFF01)
    args = parser.parse_args()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"udp-bridge-{args.gateway:04x}")
    client.connect(args.broker, args.mqtt_port)
    client.loop_start()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.listen, args.udp_port))
    print(f"udp {args.listen}:{args.udp_port} -> mqtt {args.broker}:{args.mqtt_port}")

    while True:
        frame, addr = sock.recvfrom(65535)
        if len(frame) < 9:
            print(f"{addr[0]}: {len(frame)} bytes, too short for a frame")
            continue
        topic = f"roclink/in/{frame[2]:02x}/{args.gateway:04x}/up"
        client.publish(topic, frame, qos=1)
        print(f"{addr[0]}: {len(frame)} bytes -> {topic}")


if __name__ == "__main__":
    main()
