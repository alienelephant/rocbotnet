# roclink-scavbox-wifi

A demo puzzle box for the roclink scavenger hunt, and the first end-to-end example: an ESP32 box, behind an rfidprox on a XIAO ESP32-S3, over Wi-Fi to the rfgateway, through the hub to the game.

```
 ESP32 (GW-394)        XIAO ESP32-S3 rfidprox       rfgateway Pi            hub + game
 ┌────────────┐ UART  ┌──────────────────────┐ Wi-Fi ┌────────────┐ MQTT  ┌─────────────┐
 │ BOOT = the ├──────►│ login button (D0)    ├──────►│ roclink-gw ├──────►│ roclink-hub │
 │   puzzle   │◄──────┤ forwards only while  │◄──────┤ mosquitto  │◄──────┤ scavhunt    │
 └────────────┘boxlink│ a team is logged in  │roclink└────────────┘       └─────────────┘
                      └──────────────────────┘ "roclink-nodes" AP          scoreboard :8090
```

1. A player logs in at the proxy by pressing its button: it plays a fob tap with a dummy fob, `f0b00302`, and the game opens a session for that team at the **Workshop** box.
2. The box's BOOT button is the puzzle. A **short press** sends box data (how many presses, how long this one was held): the proxy forwards the bytes unchanged, the hub decodes them, and the scoreboard shows them. **Held 1.5 s**, it reports the puzzle solved: a `WIN`, which the game scores for the logged-in team.
3. Before anyone logs in, the proxy refuses both, and nothing leaves it.

## Parts

| Part | |
|---|---|
| The box | an ESP32-WROOM-32 dev board (GW-394, "ESP-32"), this repository |
| The proxy | a Seeed XIAO ESP32-S3 with its Wi-Fi antenna attached, and a push button: `rfidprox`, environment `xiao_gamebox` |
| The gateway | the rfgateway Pi (`roclink-gateway`), with its `roclink-nodes` access point |
| The backend | `roclink-hub` and `roclink-scavhunt`, configured as below |

## Wiring

| ESP32 (box) | | XIAO ESP32-S3 (proxy) |
|---|---|---|
| GPIO 17 (TX2) | → | D7 (GPIO 44, RX) |
| GPIO 16 (RX2) | ← | D6 (GPIO 43, TX) |
| GND | — | GND |
| | | D0 (GPIO 1) → push button → GND |

TX goes to RX each way. Both boards are 3.3 V and each is powered by its own USB: don't connect their 3V3 or 5V pins.

## Flashing

```bash
# the box (this repository; rfidprox must be at ../../rfidproxies/rfidprox, for the BoxLink library)
pio run -t upload && pio device monitor

# the proxy
cd ../../rfidproxies/rfidprox/firmware
pio run -e xiao_gamebox -t upload && pio device monitor -e xiao_gamebox
```

If the XIAO doesn't show up for uploading, hold BOOT, press RESET, release BOOT, and upload again.

## The backend

The proxy is the XIAO, node `0x0302`, with the demo key, which the gateway accepts as it is; in the game its box is the **Workshop**. The hub and the game are already set up for it: the hub host's configs (`infra/roclink-hubhost/roles/apps/templates/hub.toml.j2` and `game.toml.j2`) and the bench's (`infra/roclink-bench/etc/`) are the reference, so this README doesn't repeat them. They hold the `scavbox` device type, whose decoder for schema 32, `[decoder.scavbox-press]` (`struct = "<HH"`), must match `struct Press` in `src/main.cpp`; every proxy's node in its `[[node]]` list; and a `[[box]]` for each in the game. Each proxy board has its own node address (rfidprox `docs/BOXLINK.md` §9, "Node addresses").

What the game needs at run time is a team with the proxy's dummy fob, and the run started:

```bash
roclink scavhunt team xiao --name "XIAO fob" --token f0b00302
roclink scavhunt start
```

## Running the demo

| Do | The box's log | The XIAO | The scoreboard |
|---|---|---|---|
| press BOOT on the box | `press 1: refused: no one is logged in` | | |
| press the XIAO's button | `a team logged in: play!` | LED on | "XIAO fob started playing Workshop", the box card shows them playing |
| press BOOT | `press 1 (180 ms): OK` | | the Workshop card shows `presses 1 · held_ms 180` |
| hold BOOT 1.5 s | `the game says: solved` | LED off | "XIAO fob solved Workshop +100 pts" |

The XIAO's LED blinks while it has no link to the gateway, and flickers while it waits for the game. Its serial log shows every frame both ways (`[boxlink] >` and `<`).

## License

MIT, see [LICENSE](LICENSE).
