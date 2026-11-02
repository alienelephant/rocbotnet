# roclink-scavbox-picow

A demo puzzle box for the roclink scavenger hunt on a Raspberry Pi Pico 2 W: the same box as [roclink-scavbox-wifi](../roclink-scavbox-wifi), on an RP2350, behind an rfidprox. It talks boxlink to the proxy over a UART, and the proxy does the rest: logging a team in, the radio, and the game's messages. The Pico 2 W's own Wi-Fi isn't used.

```
 Pico 2 W                 rfidprox                  rfgateway Pi            hub + game
 ┌──────────────┐ UART  ┌──────────────────────┐ Wi-Fi ┌────────────┐ MQTT  ┌─────────────┐
 │ BOOTSEL = the├──────►│ login button         ├──────►│ roclink-gw ├──────►│ roclink-hub │
 │    puzzle    │◄──────┤ forwards only while  │◄──────┤ mosquitto  │◄──────┤ scavhunt    │
 └──────────────┘boxlink│ a team is logged in  │or LoRa└────────────┘       └─────────────┘
                        └──────────────────────┘                             scoreboard :8090
```

1. A player logs in at the proxy with its button: it plays a fob tap with its dummy fob (`f0b00302` on the XIAO, `f0b00301` on the Heltec), and the game opens a session for that team at the proxy's box. With no one to press it, use a XIAO with the `xiao_gamebox_auto` build: node `0x0303`, the **Lab** box, fob `f0b00303`. It logs in by itself as soon as it's on the gateway's Wi-Fi (below).
2. The Pico 2 W's BOOTSEL button is the puzzle. A **short press** sends box data (how many presses, how long this one was held): the proxy forwards the bytes unchanged, the hub decodes them, and the scoreboard shows them. **Held 1.5 s**, it reports the puzzle solved: a `WIN`, which the game scores for the logged-in team.
3. Before anyone logs in, the proxy refuses both, and nothing leaves it.

The box joins the game through its proxy alone: it says `HELLO` on the UART, and the game knows it as the proxy's node. Nothing about the Pico 2 W goes in the hub's or the game's config.

## Parts

| Part | |
|---|---|
| The box | a Raspberry Pi Pico 2 W, this repository |
| The proxy | an rfidprox game-box build: a XIAO ESP32-S3 (`xiao_gamebox`, Wi-Fi, or `xiao_gamebox_auto`, the same with no button: it logs in by itself) or a Heltec V3 (`heltec_v3_gamebox`, Wi-Fi, or `heltec_v3_gamebox_lora`) |
| The gateway and backend | as in [roclink-scavbox-wifi](../roclink-scavbox-wifi#the-backend) (XIAO) or [roclink-scavbox-lora](../roclink-scavbox-lora) (Heltec over LoRa) |

## Wiring

| Pico 2 W (box) | | XIAO ESP32-S3 (proxy) | Heltec V3 (proxy) |
|---|---|---|---|
| GP0, pin 1 (UART0 TX) | → | D7 (GPIO 44, RX) | GPIO 39 (J3-10) |
| GP1, pin 2 (UART0 RX) | ← | D6 (GPIO 43, TX) | GPIO 40 (J3-9) |
| GND, pin 3 | — | GND | GND (J3-1) |

TX goes to RX each way. All three boards are 3.3 V and each is powered by its own USB: don't connect their 3V3, VSYS or 5V pins.

BOOTSEL is the puzzle so the box needs no parts. For a button of your own, wire it from a GPIO to GND and build with `-D PIN_BUTTON=21` (or whichever free GPIO), in `build_flags`.

GP0 and GP1 are the board's default `Serial1` pins (UART0), on the Pico 2 W and the Pico W alike, so the same wiring works on a Pico W built with `board = rpipicow`. Other UART0 pins work too, with `-D PIN_BOX_TX=… -D PIN_BOX_RX=…`: TX on GP0, 12 or 16 and RX on GP1, 13 or 17 on either board, and on the Pico 2 W also TX on GP2, 14 or 18 and RX on GP3, 15 or 19.

## Flashing

```bash
# the box (this repository; rfidprox must be at ../../rfidproxies/rfidprox, for the BoxLink library)
pio run -t upload && pio device monitor

# the proxy, e.g. the XIAO (xiao_gamebox_auto: no button, it logs in by itself)
cd ../../rfidproxies/rfidprox/firmware
pio run -e xiao_gamebox -t upload && pio device monitor -e xiao_gamebox
```

The first build downloads the Raspberry Pi platform and the arduino-pico core. If the first upload finds no Pico, hold BOOTSEL while plugging it in, and upload again (or copy `.pio/build/scavbox/firmware.uf2` to the `RP2350` drive).

With the Arduino IDE instead: install the "Raspberry Pi Pico/RP2040" boards package (Earle Philhower), copy `rfidproxies/rfidprox/boxlink` into your `libraries` folder as `BoxLink`, and open `src/main.cpp` as a sketch.

## The backend

Nothing changes for the Pico 2 W: the hub knows the proxy's node, and the box's data on schema 32 is `struct Press` in `src/main.cpp`, the same as roclink-scavbox-wifi's, so the `scavbox` device type and its `[decoder.scavbox-press]` decode it as they are. The box belongs to the game as its proxy's node, and each proxy board has its own (rfidprox docs/BOXLINK.md §9, "Node addresses"): `0x0302` and the **Workshop** behind the button XIAO, `0x0303` and the **Lab** behind the XIAO that logs in by itself. The hub host's configs (infra/roclink-hubhost) list all three proxies in the hub's `[[node]]` list and give each a `[[box]]` in the game; the Lab is optional, worth points but not needed to finish. What the game needs at run time is a team with the proxy's dummy fob:

```bash
roclink scavhunt team pico --name "Pico fob" --token f0b00303   # the auto XIAO's; f0b00302 the button XIAO's
```

## Running the demo

| Do | The Pico 2 W's log | The Pico 2 W's LED | The scoreboard |
|---|---|---|---|
| press BOOTSEL | `press 1: refused: no one is logged in at the proxy` | off | |
| press the proxy's button (the auto XIAO: wait for it) | `a team logged in: play!` | on | "Pico fob started playing Lab" |
| press BOOTSEL | `press 1 (180 ms): OK` | on | the box card shows `presses 1 · held_ms 180` |
| hold BOOTSEL 1.5 s | `the game says: solved` | off | "Pico fob solved Lab +100 pts" |

If the proxy isn't up yet, the Pico 2 W says `HELLO` again every 5 s until it answers. The proxy's serial log shows every frame both ways (`[boxlink] >` and `<`).

## Unattended: no button, no reader

With the XIAO's `xiao_gamebox_auto` build, the box needs no one at the proxy. Wire the XIAO as above (nothing on D0) and flash it:

```bash
cd ../../rfidproxies/rfidprox/firmware
pio run -e xiao_gamebox_auto -t upload && pio device monitor -e xiao_gamebox_auto
```

It's node `0x0303`, so it can run beside the button XIAO at `0x0302`; never flash one address onto two running boards, or the gateway drops one board's frames as replays. Once it's on the gateway's Wi-Fi, it logs in with its dummy fob, `f0b00303`, and the Pico 2 W's LED comes on: from then on, BOOTSEL presses go to the hub and a WIN to the game. The game must have a team with that fob, and the run must be started (`roclink scavhunt start`). Until then the XIAO's log says why it's refused (`Not running`, `Unknown team`) and it tries again every 15 s. A session that ends with the box unsolved (the operator's reset, the game's timeout) is logged into again the same way. Once the box is solved it stays logged out, since a new login would only hear "already solved": restart the XIAO (its RESET button, or the power) to play again.

## License

MIT, see [LICENSE](LICENSE).
