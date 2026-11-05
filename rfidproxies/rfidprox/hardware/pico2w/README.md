# rfidprox on a Raspberry Pi Pico 2 W: the fob proxy

A scavhunt box's link to the game, on a Pico 2 W: the box talks [boxlink](../../docs/BOXLINK.md) to it over a UART, and it talks to the backend over Wi-Fi. A team logs in by tapping its 125 kHz fob on the RDM6300; the box ends the session, or the next team's fob does. Firmware: `firmware/`, environment `pico2w_fobproxy` (`src/gamebox.cpp` with `LOGIN_FOB=1`), node `0x0304`.

| File | |
|---|---|
| `bom.yaml` | the source: every part pin → net, and the breadboard layout (holes, jumpers). Edit this, then regenerate. |
| `WIRING.md` | the build guide: which hole each resistor leg goes in, then every jumper |
| `bom.md`, `bom.csv` | the bench parts list |
| `rfidprox-pico2w.vlx` | the Velxio project: the breadboard, wired |
| `rfidprox-pico2w.png` | what it looks like in Velxio |
| `rfidprox-pico2w-viewer.html` | a view-only copy to share: open it in any browser (offline), or embed it with `<iframe>`. Pan/zoom the circuit, read the code, BOM and wiring notes, download the project |

## Pins

On a breadboard (`bom.yaml` `breadboard:`; `WIRING.md` lists every hole and jumper). The Pico and the RDM6300 sit beside it on jumpers; the top rails are 5 V (tp) and GND (tn). The RDM6300 needs three wires: +5V, GND and TX.

| Pico 2 W | Pin | Goes to |
|---|---|---|
| VBUS | 40 | top + rail (5 V), and from there RDM6300 +5V (P1) |
| GND | 3 | top − rail; RDM6300 GND, the divider |
| GP5, UART1 RX | 7 | the divider's middle: 10 kΩ from RDM6300 TX, 20 kΩ to GND (5 V → 3.33 V) |
| GP0, UART0 TX | 1 | box RX |
| GP1, UART0 RX | 2 | box TX |
| GND | any | box GND |

The RDM6300 runs on 5 V and its TX swings to 5 V: **never wire it straight to a Pico pin.** The box port's pins are those of [roclink-scavbox-picow](../../../../scavboxes/roclink-scavbox-picow), so the same cable works (proxy TX to box RX, both 3.3 V, GND shared, each board on its own USB). The onboard LED shows the state. GP4 is UART1's TX, which the firmware claims and leaves unconnected.

## The firmware

```bash
cd ../../firmware
~/.platformio/penv/bin/pio run -e pico2w_fobproxy -t upload     # hold BOOTSEL while plugging in if no Pico is found
~/.platformio/penv/bin/pio device monitor -e pico2w_fobproxy
```

It joins the rfgateway's `roclink-nodes` Wi-Fi (`config.h`; override in `secrets.h`), checks the gateway with a `PING`, and waits for a fob. Then:

1. A fob is tapped: the proxy sends the game a `START` with the fob's ID as the token.
2. The game says go: the proxy opens a session to the box (`SESSION_OPEN`, label `fob 0f00a1b2c3`). The box's data and its game events (win, wrong, hint, end) go to the game with that token, and the verdicts come back to the box.
3. The session ends when the box ends it (solved, gave up), or when another team's fob is tapped: the first team gets an `END`, and the new fob logs in. The fob already logged in, tapped again, changes nothing.

The box for this proxy can be the XIAO ESP32-S3 build of `roclink-scavbox-wifi` (`pio run -e xiao`: a button on D0, hold 5 s to win); its README has the wiring and what each side logs.

Each team's fob ID is its token in the game. Tap the fob once and read the ID from the serial log, then:

```bash
roclink scavhunt team red --name "Red" --token 0f00a1b2c3
```

The game's box for this proxy is `gate` at node `0x0304` (infra `game.toml`; the hub's `scavbox` node list has the address too).

| The onboard LED | Means |
|---|---|
| blinking (1 Hz) | no link: joining Wi-Fi, or the gateway doesn't answer |
| off | ready: tap a fob |
| flickering | waiting for the game's verdict |
| on | a team is logged in |

The serial log at start, then on a tap. The start-up, security and reader lines are as seen in Velxio's Pico simulator, with a stand-in RDM6300 that sends a tag; the Wi-Fi, gateway and game lines are what the code prints, not yet seen on hardware:

```
[scavbox] firmware 0001, node 42/0304, logins: a tapped fob; box port RX GPIO1, TX GPIO0
[rdm6300] TX to GPIO5 through the divider, 9600 baud; line HIGH: the reader is powered and its TX reaches the pin
[link] node 42/0304, epoch 0 (DEMO key)
[link] joining Wi-Fi roclink-nodes
[link] Wi-Fi up: 10.42.0.x, gateway 10.42.0.1:3333
[scavbox] PING ACKed: ready
[rdm6300] fob 0F00A1B2C3 (EM4100: id 10597059)
[game] fob 0F00A1B2C3: logging in
[game] START sent and ACKed
[game] VERDICT result 6, solved 0 of 3, arg 0
[boxlink] < SESSION_OPEN 1
```

Every 5 s the log says what arrived from the reader, or with nothing, the level of the GP5 line: `HIGH` means the reader is powered and its TX reaches the pin through the divider; `LOW` means it doesn't (`RDM6300_DEBUG` in `config.h`; docs/BREADBOARD.md §6 has the troubleshooting table).

**Pico specifics.** The roclink key and frame counter live in an EEPROM-emulation flash sector, erased and rewritten whole on each save, so an old key doesn't linger (`src/nvstore.h`; roclink SECURITY.md R4). roclink's AES comes from BearSSL, which the Pico core links anyway (`src/crypto_pico.cpp`), checked against the AES test vectors when security starts. Wi-Fi joins in the background (`beginNoBlock`), so the reader and the box are served while it joins.

## Opening it in Velxio

In the editor, use the **Open project** button (folder icon, top of the file explorer) and pick `rfidprox-pico2w.vlx`. Velxio has no Pico 2 W, so the Pico W stands in: same pinout and header numbering. The RDM6300 is a layout-only custom chip: it matches the module's pinout (P1: TX RX NC GND +5V; P3: LED +5V GND; P2: the coil), but simulates nothing. Its header positions are laid out for wiring, not to scale. Below the circuit is the Pico 2 W pinout as a reference card (`REF1`): an image only, not in the BOM.

## Regenerating

With the `velxio-bom` Claude skill (`~/.claude/skills/velxio-bom`):

```bash
python3 scripts/bom2vlx.py <this dir>/bom.yaml     # validates, rewrites the .vlx, WIRING.md and BOMs
node scripts/verify.mjs <this dir>/rfidprox-pico2w.vlx --viewer   # checks it in the running Velxio, rewrites the viewer
```

When the prototype moves to a PCB, `bom.yaml`'s pin → net maps carry over to the `pcb-design` skill's netlist.
