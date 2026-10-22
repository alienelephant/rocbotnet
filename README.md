# robo_rf_gateways

Everything for the roclink scavenger hunt: the protocol, the gateway, the hub and game, the proxies, and the boxes, in one repository. Clone it as `~/rocbotnet` on the bench Mac. The projects find each other by these relative paths, so keep the layout.

```
 scavbox ──UART──► rfidprox ──Wi-Fi or LoRa──► rfgateway (Pi) ──MQTT──► hub ──► game ──► scoreboard
 (puzzle)  boxlink  (login,      roclink,       roclink-gw,              roclink-hub   scavhunt   :8090
                    forwarding)  secured        mosquitto
```

| Folder | Project | What it is |
|---|---|---|
| `protocol/` | `roclink-rfc` | The roclink wire protocol (docs/PROTOCOL.md) and its C library, used by the proxies and the gateway |
| `rfgateways/` | `roclink-gateway` | The gateway daemon (`gw/roclink_gw.c`) and the Ansible that sets up the Pi: LoRa concentrator, MQTT broker, Wi-Fi access point, the daemon |
| `hub/` | `roclink-hub` | The hub: MQTT in, SQLite, routing to services, downlink, declared payload decoders |
| `hub/` | `roclink-scavhunt` | The game: rules, the hub service, the scoreboard. `tools/scavbox.py` simulates boxes |
| `rfidproxies/` | `rfidprox` | The proxy firmware (Heltec V3/V4, XIAO ESP32-S3; Wi-Fi or LoRa), `boxlink/` (the box's library) and `tools/boxsim.py` |
| `scavboxes/` | `roclink-scavbox-wifi`, `roclink-scavbox-lora` | Demo puzzle boxes: the ESP32 behind the XIAO (Wi-Fi), the ESP32-S3 behind the Heltec (LoRa) |
| `infra/` | `roclink-bench` | This Mac's running hub and game: configs, the `roclink` script, `setup.sh`, the VS Code workspace |

## Tools on the Mac

| Tool | For | |
|---|---|---|
| PlatformIO | building and flashing firmware | VS Code's PlatformIO extension, or `~/.platformio/penv/bin/pio` |
| uv, Python 3.12 | the hub and the game | `infra/roclink-bench/setup.sh` installs them |
| Ansible | setting up the gateway Pi | `uv tool install --with passlib ansible` |
| git | everything | one repository: `git pull`, `git push` at the top |

The gateway's secrets are in `rfgateways/roclink-gateway/group_vars/all/vault.yml`, encrypted with the password in `rfgateways/roclink-gateway/.vault-pass`, which is never committed: copy it between machines by hand.

## Common tasks

```bash
# the hub and the game (on this Mac)
infra/roclink-bench/setup.sh               # install or update; then:
roclink restart                            # start | stop | status | logs | watch
roclink scavhunt new-run demo              # the game's operator commands
open http://localhost:8090                 # the scoreboard

# a proxy
cd rfidproxies/rfidprox/firmware
pio run -e heltec_v3_gamebox_lora -t upload    # or xiao_gamebox, heltec_v3_gamebox
pio device monitor -e heltec_v3_gamebox_lora

# a box
cd scavboxes/roclink-scavbox-lora && pio run -t upload && pio device monitor

# the gateway Pi
cd rfgateways/roclink-gateway
ansible-playbook site.yml                  # everything
ansible-playbook site.yml --tags gw        # rebuild and restart roclink-gw
ansible-playbook site.yml --tags mqtt      # broker users and ACL

# tests
cd hub/roclink-hub && ~/roclink/venv/hub/bin/pytest
cd hub/roclink-scavhunt && ~/roclink/venv/scavhunt/bin/pytest
cd rfidproxies/rfidprox/boxlink && c++ -std=c++17 -Isrc test/test_frame.cpp src/boxlink_frame.cpp -o /tmp/t && /tmp/t
```

## The network

| | |
|---|---|
| Gateway Pi | `192.168.8.237` (Ethernet), user `botmaker`; MQTT on 1883; Wi-Fi AP `roclink-nodes` (10.42.0.1), UDP 3333 for Wi-Fi nodes; LoRa 915.0 MHz SF9 |
| This Mac | `192.168.8.112`: the hub (API on localhost:8080) and the game (scoreboard on :8090) |
| Proxies | Heltec V3 `0x0301` (LoRa) and XIAO `0x0302` (Wi-Fi), net `0x42`, demo keys; dummy fobs `f0b00301`, `f0b00302` |

Wiring for each example is in its box's README; the proxy side is in `rfidproxies/rfidprox/docs/BOXLINK.md` §9.
