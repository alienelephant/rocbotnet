# Breadboard: Heltec V3 or V4 + RC522 on Wi-Fi

The first hardware test of rfidprox: the proxy on a breadboard, with no backend. It joins the rfgateway's `roclink-nodes` Wi-Fi as roclink node `42/0301`, says `HELLO`, checks the link with `PING`s, and sends an `AUTH_REQ` for every card tapped. The gateway ACKs and publishes each one; you watch them arrive with `tcpdump` and `tools/watch.py` on the Pi.

There's no `rfidauth` yet, so no `GRANT` comes back: after 10 s the display says "No answer · no backend yet". That's the expected result for this stage.

## 1. Parts

| Part | Notes |
|---|---|
| Heltec WiFi LoRa 32 **V4** or **V3** | with its LoRa antenna on (the radio isn't used yet, but never power a LoRa board without one) |
| RC522 module | the common blue board, 13.56 MHz |
| ISO 14443A cards or fobs | MIFARE Classic 1K, Ultralight or NTAG. 125 kHz fobs don't work. |
| Buzzer (optional) | active, or a small passive piezo |
| Breadboard, jumper wires, USB-C cable | |

## 2. Wiring

### 2.1 The headers

Both boards have two 18-pin headers, **J2** and **J3**, numbered 1–18 from the USB-C end. The V4 is pin-compatible with the V3: the same signal is on the same header pin. The table below is from Heltec's WiFi LoRa 32 V4 datasheet (V4.3.1, tables 2.2.1 and 2.2.2). **Header pin numbers and GPIO numbers are different things**: GPIO 3, for example, is on J3 pin 14. The firmware uses GPIO numbers; you wire by header pin.

| Pin | J2 | J3 |
|---|---|---|
| 1 | GND | GND |
| 2 | 5V | 3V3 |
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
| 13 | **GPIO47 → RC522 SDA** | GPIO2 (V4: LoRa amplifier enable) |
| 14 | **GPIO48 → RC522 RST** | **GPIO3 → buzzer** |
| 15 | GPIO26 | **GPIO4 → RC522 MISO** |
| 16 | GPIO21: OLED reset | **GPIO5 → RC522 SCK** |
| 17 | GPIO20 (V4: USB D+) | **GPIO6 → RC522 MOSI** |
| 18 | GPIO19 (V4: USB D−) | GPIO7 (V4: LoRa amplifier supply) |

On the V3, GPIO 43/44 carry the USB serial (its CP2102), and GPIO 2, 7, 38–42 and 46 aren't used by the board. Check the silkscreen beside the headers on your board before wiring: it prints the GPIO number at each pin.

### 2.2 The RC522

Every signal is 3.3 V. **Power the RC522 from 3V3 (J3-2 or J3-3), never from 5V.**

| RC522 pin | Header pin | GPIO | `config.h` |
|---|---|---|---|
| 3.3V | J3-2 (3V3) | | |
| GND | J3-1 (GND) | | |
| SDA (chip select) | J2-13 | 47 | `PIN_RC522_SS` |
| RST | J2-14 | 48 | `PIN_RC522_RST` |
| MISO | J3-15 | 4 | `PIN_RC522_MISO` |
| SCK | J3-16 | 5 | `PIN_RC522_SCK` |
| MOSI | J3-17 | 6 | `PIN_RC522_MOSI` |
| IRQ | not connected | | |

| Other | Header pin | GPIO | Notes |
|---|---|---|---|
| Buzzer + | J3-14 | 3 | optional; − to GND (J2-1). A small passive piezo can go straight on the pin for bench use. |
| OLED | on board | 17, 18; reset 21 | powered through Vext, which the firmware switches on |
| LED | on board | 35 | flashes on each transmission |

```
   RC522                        Heltec WiFi LoRa 32 V4 / V3
  ┌────────┐
  │ 3.3V   ├──────────────────── J3-2  3V3
  │ RST    ├──────────────────── J2-14 GPIO48
  │ GND    ├──────────────────── J3-1  GND
  │ IRQ    │  (not connected)
  │ MISO   ├──────────────────── J3-15 GPIO4
  │ MOSI   ├──────────────────── J3-17 GPIO6
  │ SCK    ├──────────────────── J3-16 GPIO5
  │ SDA    ├──────────────────── J2-13 GPIO47
  └────────┘
                buzzer + ─────── J3-14 GPIO3     buzzer − ── J2-1 GND
```

### 2.3 Why these pins

- They are among the GPIOs Heltec recommends for add-ons (1, 2, 4–7, 19, 20, 47, 48), minus those either board uses: on the **V4**, GPIO 2 and 7 control the LoRa radio's power amplifier, 19 and 20 are its USB port, and 1 reads the battery.
- GPIO 38–42 are free on a V4 only while nothing is plugged into its GNSS connector, and 33–37 belong to the flash and PSRAM: none of them are used.
- The RC522 has its own SPI bus (the ESP32-S3's second SPI controller); the LoRa radio uses the first, on GPIO 8–14, inside the board.
- GPIO 3 is a strapping pin (JTAG source selection), which a buzzer can't disturb, so it gets the buzzer, not a signal the reader depends on.
- The box UART, for later, will need two more: GPIO 39 and 40 (J3-10, J3-9) are the likely pair, as long as no GNSS module is fitted to a V4.

## 3. Build and flash

The firmware is in `firmware/` (PlatformIO). It needs `roclink-rfc` next to this repository. Pick the environment for your board:

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

## 6. Reader diagnostics

With `RC522_DEBUG` on (the default in `config.h`), the serial log shows what the reader does, not just the cards it reads.

**At start:**

```
[rc522] pins: SDA/SS GPIO47, SCK GPIO5, MOSI GPIO6, MISO GPIO4, RST GPIO48
[rc522] version register 0x92: MFRC522 v2.0
[rc522] self-test: PASS
[rc522] antenna ON (TxControlReg 0x83), receiver gain 0x70 (max 0x70)
```

Then a register probe: the chip's command and status registers, a write test (two patterns written to a register and read back), and a `VERDICT` line:

```
[rc522] registers: Command 0x20 (PowerDown off, command 0x0), Status1 0x21, Status2 0x00, Error 0x00, RFCfg 0x70, TxControl 0x83
[rc522] write test: wrote 0x5A, read back 0x5A: ok
[rc522] write test: wrote 0xA5, read back 0xA5: ok
[rc522] VERDICT: the chip is written to and its antenna is on.
```

**Every 5 seconds,** a summary of the polls since the last one, a fresh read of the version register (so a loose wire shows), and the link state:

```
[rc522] last 5s: 1650 polls, 1650 silent, 0 card answers, 0 UID failures, 0 errors | version 0x92, antenna on | link: ready
```

**Whenever something answers:**

```
[rc522] a card answered REQA: ATQA 0004; reading its UID
[rc522] card 0A1B2C3D (ATQA 0004, SAK 08)
```

| What you see | Means | Do |
|---|---|---|
| `version register 0x00` or `0xFF: NO ANSWER on SPI` | The chip isn't on the bus | Check each wire against §2.2 by header pin; MISO and MOSI swapped is the usual one; 3.3 V and GND |
| The version changes between reports, `(CHANGED: loose wire?)` | A connection comes and goes | Reseat the jumpers; breadboard contacts wear |
| `antenna OFF`, with `write test … FAILED` | The chip answers reads but ignores writes. Usually it hasn't finished its reset because its oscillator isn't running: **no power on the module's 3.3V pin** (it runs weakly off the signal lines), no GND, or a dead crystal. The `VERDICT` line says which. | Measure the voltage between the module's 3.3V and GND pins with the board on: it should be 3.3 V. Reseat both wires, ideally straight to the board's J3-2 and J3-1. Then check MOSI and SCK. |
| `the chip RESET itself after the antenna turned on`, or `antenna is off again … it RESET` every 5 s | Writes work, but when the antenna turns on (the module's biggest load, 50–100 mA) its 3.3 V supply dips, the chip browns out and returns to its power-on settings (TxControl `0x80`, RFCfg `0x48`): antenna off, no field, no cards | Power the module straight from J3-2/J3-3 (3V3) and J3-1 (GND) with short wires, not through breadboard rails; add a 10–100 µF capacitor across its 3.3V and GND pins; measure 3.3 V there while it runs. The firmware re-initialises it each time, so cards read once the supply holds. |
| `antenna OFF`, write test ok, no reset | The antenna driver didn't turn on | Reset the board; if it persists, the module's antenna circuit is suspect |
| `self-test: FAIL` with version `0x88`, `0x12` or `0xB2` | A clone chip | Normal; carry on with the other checks |
| Polls but all `silent` while you hold a card on it | Nothing in the field answers | Hold the card flat on the antenna (the coil on the board, not the chip) for a second or two. Check it's a 13.56 MHz ISO 14443A card: 125 kHz fobs (EM4100, often grey teardrops marked "125K") never answer. Try a bank card or a phone with NFC as a known-good tag: either should at least produce `errors` or `card answers`. |
| `errors` (e.g. `CRC error`, `A buffer is not big enough`) | Something answers but garbled | The card is at the edge of the field, or the module's antenna is weak: hold it closer and still |
| `card answers` but `UID failures` | A card answered, but its UID read failed | Hold it still and flat; one card at a time |
| `NOT POLLED` | The loop isn't reaching the reader | Shouldn't happen: report it with the log |
| `link: joining Wi-Fi` or `waiting for HELLO ACK` | The reader still runs; cards show as "Offline" on the display, and aren't sent | Fix the link separately (§5) |

To turn the detail off, `#define RC522_DEBUG 0` in `secrets.h`. The RC522's SPI clock is 1 MHz (`RC522_SPI_HZ`), slow enough for breadboard wires.

## 7. Troubleshooting

| Symptom | Check |
|---|---|
| `RC522 not found` on the display | Wiring by header pin (§2.1), especially MISO/MOSI swapped, and 3.3 V. The serial log shows the version register value. |
| Blank display on a V4 | Vext's polarity: the firmware drives GPIO36 high on a V4 (from the datasheet) and low on a V3. Check you flashed the right environment. |
| No serial output on a V4 | Its serial is the native USB port: use `-e heltec_v4`, whose build turns USB CDC on. |
| Card taps do nothing | Hold the card flat against the antenna for a moment. A tap counts once until the card leaves the field. 125 kHz fobs aren't read at all. |
| `Wi-Fi: joining` forever | `roclink-nodes` reachable? `sudo hostapd_cli -i wlan0 all_sta` on the Pi lists connected stations. |
| `No gateway` | `roclink-gw` running on the Pi; `tcpdump` shows whether frames arrive. |
| Frames arrive but no ACK, `DROPPED … replay` | The proxy's frame counter is behind what the gateway stored for 0x0301, e.g. after erasing the board's flash. Give it another node address, or reset the gateway's state for net 0x42 (roclink-gateway `docs/OPERATIONS.md`). |
| `DROPPED … auth` | The node's key doesn't match the gateway's master key: a `secrets.h` key from another master. |
