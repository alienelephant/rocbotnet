# Breadboard: Heltec V3 + RC522 on Wi-Fi

The first hardware test of rfidprox: the proxy on a breadboard, with no backend. It joins the rfgateway's `roclink-nodes` Wi-Fi as roclink node `42/0301`, says `HELLO`, checks the link with `PING`s, and sends an `AUTH_REQ` for every card tapped. The gateway ACKs and publishes each one; you watch them arrive with `tcpdump` and `tools/watch.py` on the Pi.

There's no `rfidauth` yet, so no `GRANT` comes back: after 10 s the display says "No answer · no backend yet". That's the expected result for this stage.

## 1. Parts

| Part | Notes |
|---|---|
| Heltec WiFi LoRa 32 V3 | with its antenna on (the LoRa radio isn't used yet, but never run it without one) |
| RC522 module | the common blue board, 13.56 MHz |
| ISO 14443A cards or fobs | MIFARE Classic 1K, Ultralight or NTAG. 125 kHz fobs don't work. |
| Buzzer (optional) | active, or a small passive piezo |
| Breadboard, jumper wires, USB-C cable | |

## 2. Wiring

Every signal is 3.3 V. **Power the RC522 from 3V3, never 5 V.**

| RC522 pin | Heltec V3 pin | GPIO | `config.h` |
|---|---|---|---|
| 3.3V | 3V3 | | |
| GND | GND | | |
| SDA (chip select) | 4 | 4 | `PIN_RC522_SS` |
| SCK | 5 | 5 | `PIN_RC522_SCK` |
| MOSI | 6 | 6 | `PIN_RC522_MOSI` |
| MISO | 7 | 7 | `PIN_RC522_MISO` |
| RST | 3 | 3 | `PIN_RC522_RST` |
| IRQ | not connected | | |

| Other | Heltec V3 pin | GPIO | Notes |
|---|---|---|---|
| Buzzer + | 2 | 2 | optional; − to GND. A passive piezo can go straight on the pin for bench use. |
| OLED | on board | 17, 18, 21; Vext 36 | the firmware switches Vext on |
| LED | on board | 35 | flashes on each transmission |

```
   RC522                     Heltec WiFi LoRa 32 V3
  ┌────────┐
  │ 3.3V   ├──────────────── 3V3
  │ RST    ├──────────────── GPIO 3
  │ GND    ├──────────────── GND
  │ IRQ    │  (n/c)
  │ MISO   ├──────────────── GPIO 7
  │ MOSI   ├──────────────── GPIO 6
  │ SCK    ├──────────────── GPIO 5
  │ SDA    ├──────────────── GPIO 4
  └────────┘                 GPIO 2 ──── buzzer + (optional)
```

The pin labels are printed beside the V3's headers. GPIO 1–7 are on one header row; check the silkscreen on your board's revision before wiring.

**Why these pins.** The RC522 has its own SPI bus (the ESP32-S3's second SPI controller), because the LoRa radio uses the first one on GPIO 8–14. GPIO 3 is a strapping pin, but only for JTAG selection, so it's safe as an output. The box UART (GPIO 47/48, HARDWARE.md §2.2) isn't used yet.

### The full pin map

| GPIO | Use | Where |
|---|---|---|
| 0 | PRG button | on board |
| 1 | battery voltage | on board |
| 2 | buzzer | breadboard |
| 3 | RC522 RST | breadboard |
| 4 | RC522 SDA (chip select) | breadboard |
| 5 | RC522 SCK | breadboard |
| 6 | RC522 MOSI | breadboard |
| 7 | RC522 MISO | breadboard |
| 8–14 | SX1262 LoRa radio (NSS, SCK, MOSI, MISO, RST, BUSY, DIO1) | on board |
| 17, 18, 21 | OLED SDA, SCL, RST | on board |
| 35 | LED | on board |
| 36 | Vext (low = OLED and Vext pin powered) | on board |
| 43, 44 | USB serial (CP2102) | on board |
| 47, 48 | box UART TX, RX (later) | reserved |

## 3. Build and flash

The firmware is in `firmware/` (PlatformIO). It needs `roclink-rfc` next to this repository.

```bash
cd ~/Documents/code/ai_slop/rfidprox/firmware
~/.platformio/penv/bin/pio run -e heltec_v3 -t upload
~/.platformio/penv/bin/pio device monitor -e heltec_v3
```

Defaults, in `firmware/include/config.h`:

| Setting | Value |
|---|---|
| Wi-Fi | `roclink-nodes` / `roclink-demo` |
| Gateway | `10.42.0.1:3333` |
| roclink address | net `0x42`, node `0x0301` |
| Key | the DEMO key for `0x0301` (the gateway holds the demo master key, so it accepts it) |
| PING | every 30 s, with `ACK_REQ` |

To override any of them, create `firmware/include/secrets.h`, which is git-ignored, with `#define`s, e.g. your own key from `roclink-rfc/tools/provision.py`.

## 4. What you should see

**Serial monitor:**

```
[rfidprox] firmware 0001, node 42/0301
[rc522] version register 0x92: found
[link] node 42/0301, epoch 0, joining roclink-nodes (DEMO key)
[link] Wi-Fi up: 10.42.0.x, gateway 10.42.0.1:3333
[rfidprox] HELLO ACKed: ready
[rc522] card 04A1B2C3D4E5F6 (ATQA 0044, SAK 00)
[auth] AUTH_REQ 1 sent and ACKed; waiting for a GRANT
[auth] no GRANT: is rfidauth running? (expected on the breadboard)
[link] PING: ACKed
```

The RC522's version register reads `0x91` or `0x92` on a genuine NXP chip; clones often read `0x88`, `0x12` or `0xB2`, and work the same. `0x00` or `0xFF` means it isn't answering: check the wiring and 3.3 V.

**Display:** the node address; the link (how long since the last ACK); counters (frames sent, ACKs, downlinks); then the current message: "Tap a card", "Checking…", "No answer".

## 5. Watching the traffic on the rfgateway

### Decoded: what the gateway accepted and published

The gateway verifies and decrypts each frame, then publishes it on MQTT. `tools/watch.py` decodes the proxy's messages. Copy it to the Pi once, then:

```bash
scp -o ControlPath=/tmp/.botssh2 tools/watch.py x:              # from this repo
mosquitto_sub -h localhost -u admin -P 'ADMIN_PASSWORD' -t 'roclink/in/#' -v | python3 watch.py   # on the Pi
```

(The admin password: `ansible-vault view group_vars/all/vault.yml` in roclink-gateway.)

```
15:12:37 42/0x0301 seq 5 fcnt 1 [enc] via udp: HELLO: firmware 0001, radio Wi-Fi UDP, box (none)
15:12:41 42/0x0301 seq 6 fcnt 2 [enc] via udp: AUTH_REQ: req 1, card UID 04A1B2C3D4E5F6 (7 bytes), ATQA 0044, SAK 00, method 0
15:13:07 42/0x0301 seq 7 fcnt 3 [enc] via udp: PING: link check
```

A frame the gateway refused shows as `DROPPED … replay`, `auth`, etc.

### Raw: the frames on the air

```bash
sudo tcpdump -i wlan0 -n -X udp port 3333        # on the Pi
```

Payloads are encrypted, but the 9-byte header and security header are in the clear:

| Byte | Field | A proxy uplink | The gateway's ACK |
|---|---|---|---|
| 0 | version and type | `20` (DATA) | `24` (ACK) |
| 1 | flags | `83`: secured, `ACK_REQ`, `LISTEN` | `80`: secured |
| 2 | net | `42` | `42` |
| 3–4 | source, little-endian | `01 03` (node 0x0301) | `01 FF` (gateway 0xFF01) |
| 5–6 | destination | `00 00` (any gateway) | `01 03` |
| 7 | seq | increments | echoes the uplink's |
| 8 | schema | `01` HELLO, `02` AUTH_REQ, `00` PING | `00` |
| 9 | security control | `30`: encrypted, 8-byte tag | `30` |
| 10–13 | frame counter, little-endian | increments, survives reboots | the gateway's own |

Each uplink should be followed within a few milliseconds by the gateway's ACK to `10.42.0.x:3333`.

### The gateway's log

```bash
journalctl -u roclink-gw -f        # on the Pi
```

```
[info] net 0x42: new node 0301
[info] up 42/0301 seq 6 schema 2 [enc] 13 bytes via udp
```

## 6. Troubleshooting

| Symptom | Check |
|---|---|
| `RC522 not found` on the display | Wiring, especially MISO/MOSI swapped, and 3.3 V. The serial log shows the version register value. |
| Card taps do nothing | Hold the card flat against the antenna for a moment. A tap counts once until the card leaves the field. 125 kHz fobs aren't read at all. |
| `Wi-Fi: joining` forever | `roclink-nodes` reachable? `sudo hostapd_cli -i wlan0 all_sta` on the Pi lists connected stations. |
| `No gateway` | `roclink-gw` running on the Pi; `tcpdump` shows whether frames arrive. |
| Frames arrive but no ACK, `DROPPED … replay` | The proxy's frame counter is behind what the gateway stored for 0x0301, e.g. after erasing the board's flash. Give it another node address, or reset the gateway's state for net 0x42 (roclink-gateway `docs/OPERATIONS.md`). |
| `DROPPED … auth` | The node's key doesn't match the gateway's master key: a `secrets.h` key from another master. |
