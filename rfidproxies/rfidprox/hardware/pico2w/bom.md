# BOM: rfidprox-pico2w

| Ref | Qty | Item | Value | Function | In Velxio | Notes |
|---|---|---|---|---|---|---|
| U1 | 1 | Raspberry Pi Pico W |  | Microcontroller board | yes |  |
| BB1 | 1 | Breadboard (full) |  | Holds the divider and LED; jumpers to the board and modules | yes | 7 jumper wires |
| U2 | 1 | RDM6300 |  | 125 kHz EM4100 reader, 9600 baud UART out (layout-only custom part) | yes | Runs on 5 V and its TX swings to 5 V; never wire it straight to a Pico pin. P1 order on clones varies; check the silkscreen. |
| R1 | 1 | Resistor (custom) | value=10000 | Divider top, RDM6300 TX to GP5 | yes |  |
| R2 | 1 | Resistor (custom) | value=20000 | Divider bottom, GP5 to GND (5 V to 3.33 V) | yes |  |
| L1 | 1 | RDM6300 coil antenna (ships with the module) |  | 125 kHz field | no | Plugs into P2. Keep it away from the Pico's Wi-Fi antenna and from metal. |
| J2 | 1 | Boxlink port (3-pin header to the puzzle box) |  | UART0 115200 baud to the box (docs/BOXLINK.md) | no | Cross over at the box (proxy TX to box RX). Both sides 3.3 V; don't join their 3V3/VSYS. |
|  | 3 | EM4100 125 kHz test fobs/cards |  |  | no |  |
|  | 4 | Jumper wires, M-M (breadboard, Pico on headers) |  |  | no |  |
|  | 3 | Jumper wires, M-F (RDM6300 header to breadboard) |  |  | no |  |
|  | 1 | USB cable for the Pico 2 W (micro-USB) |  |  | no |  |
