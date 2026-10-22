# roclink-scavbox-lora

A demo puzzle box for the roclink scavenger hunt, and the LoRa end-to-end example: an ESP32-S3 box, behind an rfidprox on a Heltec WiFi LoRa 32 V3, over LoRa to the rfgateway, through the hub to the game. It's the same box as [roclink-scavbox-wifi](../roclink-scavbox-wifi) on another board; only the proxy's radio differs, and the box doesn't know which one it is.

```
 ESP32-S3 DevKit         Heltec V3 rfidprox          rfgateway Pi             hub + game
 ┌────────────┐ UART  ┌──────────────────────┐ LoRa  ┌──────────────┐ MQTT  ┌─────────────┐
 │ BOOT = the ├──────►│ PRG = login          ├──────►│ SX1302 HAT   ├──────►│ roclink-hub │
 │   puzzle   │◄──────┤ forwards only while  │◄──────┤ roclink-gw   │◄──────┤ scavhunt    │
 └────────────┘boxlink│ a team is logged in  │915 MHz│ mosquitto    │       └─────────────┘
                      └──────────────────────┘  SF9  └──────────────┘        scoreboard :8090
```

1. A player logs in at the proxy with its **PRG** button: it plays a fob tap with a dummy fob, `f0b00301`, and the game opens a session for that team at the **Tower** box.
2. The box's BOOT button is the puzzle. A **short press** sends box data (how many presses, how long this one was held): the proxy forwards the bytes unchanged over LoRa, the hub decodes them, and the scoreboard shows them. **Held 1.5 s**, it reports the puzzle solved: a `WIN`, which the game scores for the logged-in team.
3. Before anyone logs in, the proxy refuses both, and nothing goes on the air.

## Parts

| Part | |
|---|---|
| The box | an ESP32-S3-DevKitC-1 (two USB-C ports), this repository |
| The proxy | a Heltec WiFi LoRa 32 V3 with its LoRa antenna attached: `rfidprox`, environment `heltec_v3_gamebox_lora` |
| The gateway | the rfgateway Pi with its SX1302 LoRa HAT (`roclink-gateway`): 915.0 MHz, SF9, 125 kHz |
| The backend | `roclink-hub` and `roclink-scavhunt`, configured as below |

## Wiring

| ESP32-S3 DevKit (box) | | Heltec V3 (proxy) |
|---|---|---|
| GPIO 17 (J1 pin 10) | → | GPIO 39 (J3-10) |
| GPIO 18 (J1 pin 11) | ← | GPIO 40 (J3-9) |
| GND | — | GND (J3-1) |

TX goes to RX each way. Both boards are 3.3 V and each is powered by its own USB: don't connect their 3V3 or 5V pins. Check the silkscreen: the GPIO number is printed beside each header pin on both boards. Never power the Heltec's radio without its antenna.

## Flashing

```bash
# the box (this repository; rfidprox must be next to it, for the BoxLink library).
# Use the DevKit's port marked UART: the box's log is on it.
pio run -t upload && pio device monitor

# the proxy
cd ../rfidprox/firmware
pio run -e heltec_v3_gamebox_lora -t upload && pio device monitor -e heltec_v3_gamebox_lora
```

The DevKit's RGB LED is green while a team is logged in. On a v1.1 DevKit the LED is on GPIO 38, not 48: build with `-D RGB_LED_PIN=38`.

## The backend

The proxy is node `0x0301` with the demo key, which the gateway accepts as it is. The hub's device type for the box is the same as the Wi-Fi example's, `scavbox`, since it's the same box firmware: its decoder for schema 32 must match `struct Press` in `src/main.cpp`.

```toml
[decoder.scavbox-press]
struct = "<HH"                       # uint16 presses, uint16 held_ms
fields = ["presses", "held_ms"]

[[device_type]]
name = "scavbox"
class = "always_listening"
schemas = { 32 = "scavbox-press" }

[[node]]
net = 0x42
nodes = ["0x0301", "0x0302"]         # the Heltec (LoRa) and the XIAO (Wi-Fi) proxies
type = "scavbox"
```

The game needs the box and a team with the dummy fob:

```toml
[[box]]
id = "tower"
name = "Tower (Heltec LoRa)"
net = 0x42
node = 0x0301
```

```bash
scavhunt team heltec --name "Heltec fob" --token f0b00301
```

## Running the demo

| Do | The box's log | The Heltec | The scoreboard |
|---|---|---|---|
| press BOOT on the box | `press 1: refused: no one is logged in` | | |
| press the Heltec's PRG | `a team logged in: play!` | "Go!" | "Heltec fob started playing Tower", the box card shows them playing |
| press BOOT | `press 1 (180 ms): OK` | `[lora] received …` | the Tower card shows `presses 1 · held_ms 180` |
| hold BOOT 1.5 s | `the game says: solved` | "Solved!" | "Heltec fob solved Tower +200 pts" |

The Heltec's OLED shows the session (`IN`/`out`), the last ACK, and the signal of the last LoRa frame it heard. Its serial log shows every boxlink frame and every LoRa packet with its RSSI and SNR; the gateway's log (`journalctl -u roclink-gw -f` on the Pi) shows each uplink `via lora` and each verdict `acked`.

**Timing.** Over LoRa each frame takes about 0.2 s of airtime at SF9, and the gateway answers 150 ms after it ends, so a press or a WIN takes about half a second to be acknowledged, and the verdict a little longer.

## License

MIT, see [LICENSE](LICENSE).
