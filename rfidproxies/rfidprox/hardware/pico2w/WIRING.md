# Wiring: rfidprox-pico2w

rfidprox on a Raspberry Pi Pico 2 W, on a breadboard: the fob proxy (firmware environment pico2w_fobproxy). An RDM6300 125 kHz reader on UART1, three wires (+5V, GND, TX, its TX through a 10k/20k divider to GP5), and the boxlink port to the puzzle box on UART0 (GP0/GP1, as in roclink-scavbox-picow). Velxio has no Pico 2 W: the Pico W stands in (same pinout and header numbering).

## Connections

All made by the breadboard layout (Breadboard build, below), and wired in Velxio.

- [ ] **ANT1**: U2.ANT1 ↔ L1.1 (off-canvas)
- [ ] **ANT2**: U2.ANT2 ↔ L1.2 (off-canvas)
- [ ] **BOX_RX**: U1.GP1 ↔ J2.RX (off-canvas)
- [ ] **BOX_TX**: U1.GP0 ↔ J2.TX (off-canvas)
- **RDM_RX**: U1.GP5 ↔ R1.2 ↔ R2.1
- **RDM_TX**: U2.TX ↔ R1.1

## Power

- **5V**: U1.VBUS, U2.+5V
- **GND**: U1.GND.1, U2.GND, R2.2

## Off-canvas parts (no Velxio model, wire on the bench)

- [ ] **L1** RDM6300 coil antenna (ships with the module): 1→ANT1, 2→ANT2
  - Plugs into P2. Keep it away from the Pico's Wi-Fi antenna and from metal.
- [ ] **J2** Boxlink port (3-pin header to the puzzle box): TX→BOX_TX, RX→BOX_RX, GND→GND
  - Cross over at the box (proxy TX to box RX). Both sides 3.3 V; don't join their 3V3/VSYS.

## Breadboard build

BB1: Breadboard (full). Holes are `<column><t|b>.<row>`; a 5-hole strip (e.g. 10t.a-e) is one connection, each rail one long one.

### Plug in

- [ ] **R1** (Resistor (custom)): 1→51t.c, 2→57t.c
- [ ] **R2** (Resistor (custom)): 1→57t.d, 2→63t.d

### Jumpers

- [ ] U1.VBUS → BB1.tp.50 (red)
- [ ] U1.GND.1 → BB1.tn.50 (black)
- [ ] U2.+5V → BB1.tp.19 (red)
- [ ] U2.GND → BB1.tn.18 (black)
- [ ] U2.TX → BB1.51t.a (yellow)
- [ ] U1.GP5 → BB1.57t.e (green)
- [ ] BB1.63t.a → BB1.tn.49 (black)

## Parts

- U1 (Raspberry Pi Pico W)
- U2 (RDM6300) - Runs on 5 V and its TX swings to 5 V; never wire it straight to a Pico pin. P1 order on clones varies; check the silkscreen.
- R1 (Resistor (custom))
- R2 (Resistor (custom))
