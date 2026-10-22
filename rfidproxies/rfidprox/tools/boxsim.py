#!/usr/bin/env python3
"""boxsim: plays a puzzle box over boxlink (docs/BOXLINK.md), for testing a proxy.

Speaks boxlink on a serial port: the proxy's USB port (its game-box build
accepts frames there too, and prints its log between them), or a 3.3 V
USB-UART adapter on its box port.

    python tools/boxsim.py /dev/ttyUSB0

Then type commands, or pipe them in (each waits for the proxy's answer):

    hello vault 1             HELLO: box type and version
    ping                      PING: the proxy answers with its STATUS
    data 32 a7080000004242    the box's own bytes on schema 32-255
    pack 32 <hfB 2215 48.5 1  the same, from a Python struct format and values
    win | fail | end          the game's events
    hint [N]                  a hint (the next one without N)
    wait 2                    pause, in seconds; also shows what arrived
    prg [SECONDS]             press the proxy's PRG button (default 0.2 s; 2 logs out),
                              through the CP2102's DTR line: a Heltec V3 on its USB port only

Needs pyserial.
"""

from __future__ import annotations

import argparse
import struct
import sys
import threading
import time

import serial

VERSION = 1
DATA, ACK, CTRL = 0, 1, 2
ACK_REQ = 0x01
OPS = {1: "HELLO", 2: "HELLO_ACK", 3: "PING", 4: "STATUS", 5: "SESSION_OPEN", 6: "SESSION_CLOSED",
       7: "END_SESSION", 8: "DENIED", 9: "TRIGGER", 10: "VERDICT"}
OP = {v: k for k, v in OPS.items()}
TRIGGERS = {"win": 1, "fail": 2, "hint": 3, "end": 4}
REFUSALS = {1: "no_session", 2: "busy", 3: "too_long", 4: "bad_schema", 5: "stale", 6: "offline",
            7: "unknown"}
RESULTS = {0: "solved", 1: "solved, finished", 2: "already solved", 3: "locked", 4: "unknown team",
           5: "not running", 6: "go", 7: "busy", 8: "no session", 9: "hint granted", 10: "no more hints"}
LINKS = {0: "down", 1: "up", 2: "grace"}
CLOSE_REASONS = {1: "box_ended", 2: "revoked", 3: "expired", 4: "link_lost", 5: "box_lost",
                 6: "proxy_restart"}


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def cobs_encode(data: bytes) -> bytes:
    out, block = bytearray(), bytearray()
    for b in data:
        if b == 0:
            out += bytes([len(block) + 1]) + block
            block.clear()
        else:
            block.append(b)
            if len(block) == 254:
                out += b"\xff" + block
                block.clear()
    return bytes(out + bytes([len(block) + 1]) + block)


def cobs_decode(data: bytes) -> bytes | None:
    out, i = bytearray(), 0
    while i < len(data):
        code = data[i]
        if code == 0 or i + code > len(data):
            return None
        out += data[i + 1 : i + code]
        i += code
        if code != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


def encode(ftype: int, flags: int, seq: int, schema: int, payload: bytes) -> bytes:
    raw = bytes([VERSION << 5 | ftype << 2, flags, seq, schema]) + payload
    return b"\x00" + cobs_encode(raw + struct.pack("<H", crc16(raw))) + b"\x00"


def decode(chunk: bytes):
    raw = cobs_decode(chunk)
    if raw is None or len(raw) < 6 or raw[0] >> 5 != VERSION or raw[0] & 0x03:
        return None
    if crc16(raw[:-2]) != struct.unpack("<H", raw[-2:])[0]:
        return None
    return (raw[0] >> 2) & 0x07, raw[1], raw[2], raw[3], raw[4:-2]


class Box:
    def __init__(self, port: serial.Serial, quiet_log: bool):
        self.port, self.quiet_log = port, quiet_log
        self.seq = 0
        self.answer: dict[int, int | None] = {}
        self.lock = threading.Lock()
        threading.Thread(target=self._read, daemon=True).start()

    def say(self, text: str) -> None:
        with self.lock:
            print(text, flush=True)

    def _read(self) -> None:
        buf = bytearray()
        while True:
            data = self.port.read(256)
            for b in data:
                if b != 0:
                    buf.append(b)
                    continue
                if buf:
                    frame = decode(bytes(buf))
                    if frame:
                        self._frame(*frame)
                    elif not self.quiet_log:  # the proxy's log, between frames
                        for line in bytes(buf).decode(errors="replace").splitlines():
                            if line.strip():
                                self.say(f"   | {line}")
                buf.clear()
            # A quiet port with whole lines waiting: log text, not a frame (a
            # frame arrives in one burst, ended by its 0x00).
            if not data and b"\n" in buf:
                text, _, rest = bytes(buf).rpartition(b"\n")
                if not self.quiet_log:
                    for line in text.decode(errors="replace").splitlines():
                        if line.strip():
                            self.say(f"   | {line}")
                buf = bytearray(rest)

    def _frame(self, ftype, flags, seq, schema, p) -> None:
        if ftype == ACK:
            self.answer[seq] = p[0] if p else 0
            return
        if ftype == DATA:
            return self.say(f"<< DATA schema {schema}: {p.hex()} (from the hub)")
        name = OPS.get(schema, f"op {schema}")
        if schema == OP["VERDICT"] and len(p) >= 5:
            result, arg, solved, required = p[0], p[1] | p[2] << 8, p[3], p[4]
            detail = f"{RESULTS.get(result, result)}, arg {arg}, solved {solved} of {required}"
        elif schema == OP["STATUS"] and len(p) >= 3:
            detail = f"link {LINKS.get(p[0], p[0])}, session {p[1] | p[2] << 8}"
        elif schema == OP["HELLO_ACK"] and len(p) >= 7:
            detail = (f"proxy firmware {p[1] | p[2] << 8:04x}, max payload {p[3]}, "
                      f"link {LINKS.get(p[4], p[4])}, session {p[5] | p[6] << 8}")
        elif schema == OP["SESSION_OPEN"] and len(p) >= 5:
            detail = f"session {p[0] | p[1] << 8}, \"{p[5:5 + p[4]].decode(errors='replace')}\""
        elif schema == OP["SESSION_CLOSED"] and len(p) >= 3:
            detail = f"session {p[0] | p[1] << 8}, {CLOSE_REASONS.get(p[2], p[2])}"
        else:
            detail = p.hex()
        self.say(f"<< {name}: {detail}")

    def request(self, ftype: int, schema: int, payload: bytes, what: str) -> None:
        """Stop-and-wait, as BoxLink does: up to 3 s per attempt, twice."""
        self.seq = (self.seq + 1) & 0xFF
        seq = self.seq
        self.answer[seq] = None
        frame = encode(ftype, ACK_REQ, seq, schema, payload)
        for attempt in range(2):
            self.port.write(frame)
            deadline = time.monotonic() + 3
            while self.answer[seq] is None and time.monotonic() < deadline:
                time.sleep(0.01)
            if self.answer[seq] is not None:
                break
            self.say(f"   (no answer to {what}{', retrying' if attempt == 0 else ''})")
        a = self.answer.pop(seq)
        if a is None:
            self.say(f">> {what}: NO ANSWER")
        else:
            self.say(f">> {what}: {'OK' if a == 0 else 'refused: ' + REFUSALS.get(a, str(a))}")

    def command(self, words: list[str]) -> None:
        op = words[0]
        if op == "hello":
            name = (words[1] if len(words) > 1 else "boxsim").encode()[:16]
            ver = int(words[2]) if len(words) > 2 else 1
            self.request(CTRL, OP["HELLO"], bytes([VERSION, len(name)]) + name + struct.pack("<H", ver), "HELLO")
        elif op == "ping":
            self.request(CTRL, OP["PING"], b"", "PING")
        elif op == "data":
            schema, payload = int(words[1]), bytes.fromhex(words[2] if len(words) > 2 else "")
            self.request(DATA, schema, payload, f"DATA {schema} ({len(payload)} bytes)")
        elif op == "pack":
            schema, fmt = int(words[1]), words[2]
            values = [float(v) if "." in v else int(v, 0) for v in words[3:]]
            payload = struct.pack(fmt, *values)
            self.request(DATA, schema, payload, f"DATA {schema} {fmt} ({payload.hex()})")
        elif op in TRIGGERS:
            arg = int(words[1]) if len(words) > 1 else 0
            self.request(CTRL, OP["TRIGGER"], bytes([TRIGGERS[op], arg]), f"TRIGGER {op}")
        elif op == "prg":
            # The V3's auto-program circuit pulls GPIO0 (PRG) low while DTR is
            # asserted and RTS isn't, as esptool does to enter the bootloader.
            held = float(words[1]) if len(words) > 1 else 0.2
            self.port.dtr = True
            time.sleep(held)
            self.port.dtr = False
            self.say(f">> PRG pressed for {held} s")
        elif op == "raw":  # a frame with any type and schema: for testing refusals
            self.request(int(words[1]), int(words[2]), bytes.fromhex(words[3] if len(words) > 3 else ""),
                         f"type {words[1]} schema {words[2]}")
        else:
            raise ValueError(f"unknown command {op!r}")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("port")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--quiet-log", action="store_true", help="don't show the proxy's log lines")
    p.add_argument("--reset", action="store_true", help="reset the board first (CP2102: RTS)")
    a = p.parse_args()
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = a.port, a.baud, 0.05
    port.dtr = port.rts = False  # opening the port mustn't reset the board
    port.open()
    if a.reset:
        port.rts = True
        time.sleep(0.1)
        port.rts = False
    box = Box(port, a.quiet_log)
    interactive = sys.stdin.isatty()
    while True:
        if interactive:
            print("> ", end="", flush=True)
        line = sys.stdin.readline()
        if not line:
            break
        words = line.split("#", 1)[0].split()
        if not words:
            continue
        if not interactive:
            box.say(f"> {' '.join(words)}")
        try:
            if words[0] == "wait":
                time.sleep(float(words[1]) if len(words) > 1 else 1)
            else:
                box.command(words)
        except (ValueError, IndexError, struct.error) as e:
            box.say(f"error: {e}")
    time.sleep(0.5)
    return 0


if __name__ == "__main__":
    sys.exit(main())
