# roclink gateway

A roclink gateway for a Raspberry Pi with an SX1302 LoRa concentrator HAT, which also serves Wi-Fi nodes over UDP, and an MQTT broker for a hub to connect to. Ansible sets it all up.

```
 LoRa nodes (915.0 MHz, SF9/125 kHz, sync word 0x12)          Wi-Fi nodes
        │                                                         │ UDP :3333
 ┌──────▼─────────────── Raspberry Pi ────────────────────────────▼──────────┐
 │ lora_pkt_fwd (Semtech sx1302_hal 2.1, SPI) ─ Semtech UDP 127.0.0.1:1700 ─┐│
 │                                                                          ▼│
 │ roclink-gw: verifies and decrypts every frame, ACKs, sends downlink in    │
 │ reply windows; publishes GATEWAY_INTERFACE.md records                     │
 │        │ roclink/in/<net>/<gw>/up | rejected | down_status                │
 │        │ roclink/out/<net>/<gw>/down                                      │
 │ mosquitto :1883 (passwords + ACL) ◄── a roclink hub connects here         │
 └───────────────────────────────────────────────────────────────────────────┘
```

| Path | What it is |
|---|---|
| `gw/roclink_gw.c` | The gateway daemon: C, built on the Pi against roclink's `src/` (from `../roclink-rfc`), libmosquitto, cJSON and OpenSSL |
| `roles/pi_base` | Turns on SPI and I2C (and reboots), installs build tools |
| `roles/mosquitto` | The broker: no anonymous access, a password per user, roclink's ACL |
| `roles/sx1302` | Semtech's HAL and packet forwarder: built from source, patched for boards without a temperature sensor, a one-channel config, a `pinctrl` reset script |
| `roles/roclink_gw` | Builds, configures and runs `roclink-gw` |
| `roles/node_ap` | The Pi's Wi-Fi as the `roclink-nodes` access point for Wi-Fi nodes (`10.42.0.1`): hostapd, DHCP from dnsmasq, firewalled so nodes reach only `roclink-gw`. The Pi's own link is Ethernet. |
| `tools/test_node.py` | A test node over UDP: sends a frame, checks the gateway's sealed ACK, receives and ACKs any downlink |

## Setting up a gateway

Needs `ansible`, the Pi reachable with a key, passwordless sudo for that user, and `../roclink-rfc` checked out next to this repo.

```bash
ansible-playbook site.yml              # everything; reboots once to turn SPI and I2C on
ansible-playbook site.yml --tags gw    # rebuild and restart just the daemon
```

`inventory.ini` lists the gateways, each with its own `gateway_id`. Settings are in `group_vars/all/main.yml`. Secrets (MQTT passwords, the network master key) are in `group_vars/all/vault.yml`, encrypted with the password in `.vault-pass`, which is git-ignored, so keep a copy of it somewhere safe. To see them: `ansible-vault view group_vars/all/vault.yml`.

## Configuration choices

- **Radio.** One multi-SF channel at 915.0 MHz, the private sync word, and no IQ inversion on transmit, to match RadioLib's defaults in roclink's sketches. Replies to an uplink are scheduled on the concentrator's clock, 150 ms after the uplink ends, then back to back.
- **Networks.** Net `0x42` is secured in master mode with open admission and require-enc, using the **demo master key** (`000102…0f`), so any node provisioned with `tools/provision.py --demo` joins. Net `0xF0` is the open test net of the unsecured sketches. Before real use, put a new master key in the vault (`tools/provision.py --new` in roclink-rfc) and re-provision the nodes. Better still, switch to enrolled mode (SECURITY.md R3), which the daemon doesn't do yet.
- **MQTT users.** `roclink-gw` (the daemon), `roclink-hub`, `roclink-ops` and `admin` (everything, for debugging). A service is a user named after its `[[route]]`; add it to `mqtt_users`.
- **The HAT.** Waveshare's SX1302 HAT differs from Semtech's CoreCell reference design in two ways. It has no STTS751 temperature sensor, which the stock HAL needs (`roles/sx1302/files/no-temp-sensor.patch` assumes 25 °C instead), and the SX1302 reset is on **GPIO17**, not 23. A CoreCell-style board needs `sx1302_pins: {reset: 23, power_en: 18, sx1261_reset: 22}` instead.

## Checking it

**[docs/OPERATIONS.md](docs/OPERATIONS.md)** covers watching MQTT and the logs, sending test traffic, and troubleshooting.

```bash
python3 tools/test_node.py --gateway 192.168.8.237          # secured, net 0x42
python3 tools/test_node.py --gateway 192.168.8.237 --open   # unsecured, net 0xF0

# on the Pi
journalctl -u roclink-gw -u lora_pkt_fwd -f
mosquitto_sub -u admin -P <password> -t 'roclink/#' -v
```

A downlink can be queued as the hub would, on `roclink/out/42/ff01/down` as user `roclink-hub`:

```json
{"v": 1, "id": "t1", "node": "0x1234", "schema": 10, "seq": 77, "ack": true,
 "payload": "0102", "send": "window", "binding": null, "expires": 1792096260000}
```

It goes out in the node's next reply window, and `roclink/in/42/ff01/down_status` reports `acked`, `sent` or `expired`.

## Not done yet

- LoRa downlink and reply timing haven't been checked on air yet. LoRa uplink works: a Heltec running `lora_secure_sender` is received.
- Enrolled mode (per-node keys instead of the master on the gateway).
- A gateway status or heartbeat topic (GATEWAY_INTERFACE.md open question 4).
