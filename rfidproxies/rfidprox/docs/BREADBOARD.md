# Breadboard: Heltec V3 or V4 + RDM6300 on Wi-Fi

The first hardware test of rfidprox: the proxy on a breadboard, with no backend. It joins the rfgateway's `roclink-nodes` Wi-Fi as roclink node `42/0301`, says `HELLO`, checks the link with `PING`s, and sends an `AUTH_REQ` for every tag held to the reader. The gateway ACKs and publishes each one; you watch them arrive with `tcpdump` and `tools/watch.py` on the Pi.

There's no `rfidauth` yet, so no `GRANT` comes back: after 10 s the display says "No answer · no backend yet". That's the expected result for this stage.

## 1. Parts

| Part | Notes |
|---|---|
| Heltec WiFi LoRa 32 **V4** or **V3** | with its LoRa antenna on (the radio isn't used yet, but never power a LoRa board without one) |
| RDM6300 module | 125 kHz, with its coil antenna; 5 V, a UART at 9600 baud |
| EM4100 tags | 125 kHz fobs or cards (often grey teardrops marked "125K"). 13.56 MHz cards (MIFARE, NTAG, bank cards, phones) don't work. |
| 2 resistors, 10 kΩ and 20 kΩ | the divider that brings the RDM6300's 5 V TX down to 3.3 V (1 kΩ and 2 kΩ work too) |
| Buzzer (optional) | active, or a small passive piezo |
| Breadboard, jumper wires, USB-C cable | |

## 2. Wiring

### 2.1 The headers

Both boards have two 18-pin headers, **J2** and **J3**, numbered 1–18 from the USB-C end. The V4 is pin-compatible with the V3: the same signal is on the same header pin. The table below is from Heltec's WiFi LoRa 32 V4 datasheet (V4.3.1, tables 2.2.1 and 2.2.2). **Header pin numbers and GPIO numbers are different things**: GPIO 3, for example, is on J3 pin 14. The firmware uses GPIO numbers; you wire by header pin.

| Pin | J2 | J3 |
|---|---|---|
| 1 | GND | GND |
| 2 | **5V → RDM6300 +5V** | 3V3 |
| 3 | Ve (switched 3.3 V) | 3V3 |
| 4 | Ve (switched 3.3 V) | GPIO37: battery-reading control |
| 5 | GPIO44 (U0RXD) | GPIO46 (V4: LoRa amplifier) |
| 6 | GPIO43 (U0TXD) | GPIO45 |
| 7 | RST (reset button) | GPIO42 (V4: GNSS reset) |
| 8 | GPIO0 (PRG button) | GPIO41 (V4: GNSS PPS) |
| 9 | GPIO36: Vext control | GPIO40 (V4: GNSS wake-up) |
| 10 | GPIO35: LED | GPIO39 (V4: GNSS TX) |
| 11 | GPIO34 | GPIO38 (V4: GNSS RX) |
| 12 | GPIO33 | GPIO1: battery voltage |
| 13 | GPIO47 | GPIO2 (V4: LoRa amplifier enable) |
| 14 | GPIO48 | **GPIO3 → buzzer** |
| 15 | GPIO26 | **GPIO4 ← RDM6300 TX (through the divider)** |
| 16 | GPIO21: OLED reset | GPIO5 |
| 17 | GPIO20 (V4: USB D+) | GPIO6 |
| 18 | GPIO19 (V4: USB D−) | GPIO7 (V4: LoRa amplifier supply) |

On the V3, GPIO 43/44 carry the USB serial (its CP2102), and GPIO 2, 7, 38–42 and 46 aren't used by the board. Check the silkscreen beside the headers on your board before wiring: it prints the GPIO number at each pin.

### 2.2 The RDM6300

The RDM6300 runs on **5 V**, and its TX line swings to 5 V. **The ESP32-S3's pins are 3.3 V only: never wire TX straight to GPIO4.** A divider (10 kΩ from TX to GPIO4, 20 kΩ from GPIO4 to GND) brings it to 3.3 V. Only TX is used: the module sends, and has nothing to be told.

| RDM6300 pin | Goes to | GPIO | `config.h` |
|---|---|---|---|
| +5V (P1) | J2-2 (5V) | | |
| GND (P1) | J2-1 (GND) | | |
| TX (P1) | 10 kΩ, then J3-15; 20 kΩ from J3-15 to GND | 4 | `PIN_RDM6300_RX` |
| RX (P1) | not connected | | |
| ANT1, ANT2 (P2) | the coil antenna | | |
| LED (P3) | optional: an LED and 1 kΩ to GND, lit while a tag is read | | |

J2-2 is the board's USB 5 V: power the board from USB. On a battery alone there is no 5 V there.

| Other | Header pin | GPIO | Notes |
|---|---|---|---|
| Buzzer + | J3-14 | 3 | optional; − to GND (J2-1). A small passive piezo can go straight on the pin for bench use. |
| OLED | on board | 17, 18; reset 21 | powered through Vext, which the firmware switches on |
| LED | on board | 35 | flashes on each transmission |

```
   RDM6300 (P1)                       Heltec WiFi LoRa 32 V4 / V3
  ┌────────┐
  │ TX     ├──[ 10k ]──┬───────────── J3-15 GPIO4
  │        │           └──[ 20k ]──┐
  │ RX     │  (not connected)      │
  │ GND    ├───────────────────────┴─ J2-1  GND
  │ +5V    ├───────────────────────── J2-2  5V
  └────────┘
   P2: ANT1, ANT2 ── the coil

                buzzer + ───────────── J3-14 GPIO3     buzzer − ── J2-1 GND
```

The pin names on RDM6300 boards vary by seller: check the silkscreen. P1 is the 5-pin header (TX, RX, NC, GND, +5V on most), P2 the antenna, P3 the LED header. Keep the coil away from the Heltec's LoRa antenna and from metal, and flat to where tags will be held.

### 2.3 Why these pins

- They are among the GPIOs Heltec recommends for add-ons (1, 2, 4–7, 19, 20, 47, 48), minus those either board uses: on the **V4**, GPIO 2 and 7 control the LoRa radio's power amplifier, 19 and 20 are its USB port, and 1 reads the battery.
- GPIO 38–42 are free on a V4 only while nothing is plugged into its GNSS connector, and 33–37 belong to the flash and PSRAM: none of them are used.
- The RDM6300 needs a single input. GPIO 4 isn't a strapping pin, so the reader's TX can't change how the board boots. The firmware reads it with UART2, leaving UART1 for the box.
- GPIO 3 is a strapping pin (JTAG source selection), which a buzzer can't disturb, so it gets the buzzer, not a signal the reader depends on.
- GPIO 5, 6, 47 and 48, which the RC522 used, are free again.
- The box UART, for later, will need two more: GPIO 39 and 40 (J3-10, J3-9) are the likely pair, as long as no GNSS module is fitted to a V4.

## 3. Build and flash

The firmware is in `firmware/` (PlatformIO). It needs `roclink-rfc` at `../../protocol/roclink-rfc` (the rocbotnet layout). Pick the environment for your board:

```bash
cd ~/Documents/code/ai_slop/rfidprox/firmware
~/.platformio/penv/bin/pio run -e heltec_v4 -t upload      # or heltec_v3
~/.platformio/penv/bin/pio device monitor -e heltec_v4
```

| | `heltec_v4` | `heltec_v3` |
|---|---|---|
| Serial monitor over | the native USB port | the CP2102 |
| Vext (OLED power) on | GPIO36 high | GPIO36 low |
| Flash, PSRAM | 16 MB, 2 MB | 8 MB, none |

If a V4 doesn't show up for uploading, hold PRG, press RST, release PRG (boot mode), then upload again.

Defaults, in `firmware/include/config.h`:

| Setting | Value |
|---|---|
| Wi-Fi | `roclink-nodes` / `roclink-demo` |
| Gateway | `10.42.0.1:3333` |
| roclink address | net `0x42`, node `0x0301` |
| Key | the DEMO key for `0x0301` (the gateway holds the demo master key, so it accepts it) |
| PING | every 30 s, with `ACK_REQ` |

To override any of them, create `firmware/include/secrets.h`, which is git-ignored, with `#define`s, e.g. your own key from `roclink-rfc/tools/provision.py`.

### The game-box build

`heltec_v3_gamebox` and `heltec_v4_gamebox` build `src/gamebox.cpp` instead of `main.cpp`: the proxy as a scavhunt box's link to the game, for the demo, before rfidauth exists. Same address and key. No card reader is used.

| Input | Does |
|---|---|
| PRG, short press | logs in with the dummy fob (`f0b00301` for node `0x0301`): a `START` |
| PRG, held 1.5 s | logs out: an `END` |
| [boxlink](BOXLINK.md) frames on the box port (GPIO 39/40), or on the USB port from `tools/boxsim.py` | box data (schemas 32–255) forwarded unchanged; win, fail, hint and end for the logged-in team (BOXLINK.md §9) |

Enter the dummy fob in the game once: `scavhunt team red --token f0b00301`. It checks the link with a `PING` instead of `HELLO`, since `HELLO`'s schema 1 is `WIN` to the game. The display shows each verdict: "Go!", "Locked: solve 0201 first", "Busy", "Solved!", and the serial log shows every frame both ways (`[boxlink] >` and `<`).

To play the box from a computer: `python tools/boxsim.py /dev/ttyUSB0`, then `hello vault 1`, `pack 32 <hfB 2215 48.5 1`, `win`, `hint`. A real box uses the `BoxLink` library in `boxlink/` (`examples/sensor_box`).

## 4. What you should see

**Serial monitor:**

```
[rfidprox] firmware 0001, node 42/0301
[rdm6300] TX to GPIO4 (through the 5 V to 3.3 V divider), 9600 baud
[link] node 42/0301, epoch 0, joining roclink-nodes (DEMO key)
[link] Wi-Fi up: 10.42.0.x, gateway 10.42.0.1:3333
[rfidprox] HELLO ACKed: ready
[rdm6300] tag 0F00A1B2C3 (EM4100: id 10597059)
[auth] AUTH_REQ 1 sent and ACKed; waiting for a GRANT
[auth] no GRANT: is rfidauth running? (expected on the breadboard)
[link] PING: ACKed
```

The tag is the EM4100's 5 bytes: a version or customer byte, then the 32-bit id, which is often printed on the fob in decimal (`10597059` here). The RDM6300 says nothing until a tag is in its field, so there is no "reader found" at start.

**Display:** the node address; the link (how long since the last ACK); counters (frames sent, ACKs, downlinks); then the current message: "Hold a tag", "Checking…", "No answer".

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
15:12:41 42/0x0301 seq 6 fcnt 2 [enc] via udp: AUTH_REQ: req 1, card UID 0F00A1B2C3 (5 bytes), EM4100 125 kHz, method 0
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

## 6. Reader diagnostics

With `RDM6300_DEBUG` on (the default in `config.h`), the serial log shows what arrives on the reader's UART, not just the tags it decodes. At start, and every 5 seconds in which nothing arrived, the level of the TX line on GPIO4:

```
[rdm6300] TX line HIGH: the reader is powered and its TX reaches GPIO4
[rdm6300] last 5s: no bytes (normal without a 125 kHz tag); TX line HIGH: ...
```

The reader's TX idles high, so GPIO4 sits at 3.3 V; a weak pull-down makes it read LOW when nothing drives it. `LOW` is a wiring or power fault, not a tag problem. Every 5 seconds in which bytes arrived:

```
[rdm6300] last 5s: 588 bytes, 42 tag frames, 0 bad frames | link: ready
```

A frame is `0x02`, ten hex digits (the tag's 5 bytes), two hex digits (their XOR), `0x03`; the module repeats it for as long as the tag stays in the field. One that fails its checksum or doesn't end in `0x03` is shown:

```
[rdm6300] bad frame: 0F00A1B2C300 (end byte 0x03)
```

| What you see | Means | Do |
|---|---|---|
| `TX line LOW` | Nothing drives GPIO4: the reader is unpowered, or its TX isn't wired to the divider | Measure 5 V between the module's +5V and GND. Check which P1 pin is TX on your module's silkscreen, and that it goes through the 10 kΩ to the junction row. |
| `no bytes` with `TX line HIGH` while you hold a tag on the coil | The reader is powered and wired, but reads nothing | Measure 5 V between the module's +5V and GND. Check TX goes to J3-15 through the 10 kΩ resistor (not RX: the pin names are close). Check the coil is plugged into P2. Check it's a 125 kHz tag: 13.56 MHz cards never answer. If the module's LED (P3) lights with a tag, the reader works and the fault is the TX wire or the divider. |
| `bytes` but every frame `bad`, often with garbage digits | The bytes arrive garbled | The divider's GND is missing, or it's wired the wrong way round (TX straight to GPIO4, 20 kΩ in the wrong place): the pin should read 3.3 V idle. |
| Some `bad frames` among good ones | Noise, usually a tag at the edge of the field | Hold the tag flat and still on the coil. Harmless: bad frames are dropped and the next good one counts. |
| Tag frames, but no `tag` line | The tag counted once already | A tag counts once while held, and again after it's been away for 2 s (`SAME_CARD_MS`). |
| `link: joining Wi-Fi` or `waiting for HELLO ACK` | The reader still runs; tags show as "Offline" on the display, and aren't sent | Fix the link separately (§5) |

To turn the detail off, `#define RDM6300_DEBUG 0` in `secrets.h`.

## 7. Troubleshooting

| Symptom | Check |
|---|---|
| Blank display on a V4 | Vext's polarity: the firmware drives GPIO36 high on a V4 (from the datasheet) and low on a V3. Check you flashed the right environment. |
| No serial output on a V4 | Its serial is the native USB port: use `-e heltec_v4`, whose build turns USB CDC on. |
| Tags do nothing | Hold the tag flat against the coil for a moment. A tag counts once until it has been away 2 s. 13.56 MHz cards aren't read at all. See §6. |
| `Wi-Fi: joining` forever | `roclink-nodes` reachable? `sudo hostapd_cli -i wlan0 all_sta` on the Pi lists connected stations. |
| `No gateway` | `roclink-gw` running on the Pi; `tcpdump` shows whether frames arrive. |
| Frames arrive but no ACK, `DROPPED … replay` | The proxy's frame counter is behind what the gateway stored for 0x0301, e.g. after erasing the board's flash. Give it another node address, or reset the gateway's state for net 0x42 (roclink-gateway `docs/OPERATIONS.md`). |
| `DROPPED … auth` | The node's key doesn't match the gateway's master key: a `secrets.h` key from another master. |
