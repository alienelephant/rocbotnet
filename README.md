# rfidprox

An RFID authentication proxy for boxes on a roclink network. A person taps a fob on the proxy; the proxy asks the backend, over encrypted roclink through the rf gateways, whether that person may use the attached box now. While a session is open, it carries the box's messages to the backend and the replies back. Without one, nothing the box says leaves the proxy.

Built for the roclink scavenger hunt, where each puzzle box gets one proxy, but the box can be any device with a UART.

**Status:** design, and a first breadboard firmware (`firmware/`, [docs/BREADBOARD.md](docs/BREADBOARD.md)): a Heltec V3 with an RC522 sending `HELLO`, `PING` and `AUTH_REQ` to the rfgateway over Wi-Fi. No backend yet.

| Document | |
|---|---|
| [docs/DESIGN.md](docs/DESIGN.md) | **Start here.** Architecture, trust model, flows, the proxy's states, the backend's parts, security, what changes elsewhere |
| [docs/BOXLINK.md](docs/BOXLINK.md) | The UART protocol between a box and its proxy |
| [docs/BACKEND.md](docs/BACKEND.md) | The roclink messages between the proxy and the backend, and the rfidauth service |
| [docs/HARDWARE.md](docs/HARDWARE.md) | Boards, wiring, power, bill of materials |
| [docs/BREADBOARD.md](docs/BREADBOARD.md) | The breadboard: wiring and pin map, flashing, and watching its traffic on the rfgateway |
| [docs/PLAN_WIFI_EXAMPLE.md](docs/PLAN_WIFI_EXAMPLE.md) | The plan for the first end-to-end example: Wi-Fi UDP through an access point on the gateway Pi |

Related repositories: `roclink-rfc` (the protocol and library), `roclink-gateway` (the Pi gateway), `roclink-scavhunt` (the game).

## License

MIT, see [LICENSE](LICENSE).
