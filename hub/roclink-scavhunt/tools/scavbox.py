#!/usr/bin/env python3
"""Simulated scavhunt boxes: secured roclink nodes over UDP, for testing the game end to end.

Each box is a roclink node on net 0x42 that sends GAME.md §6 messages to a
gateway's UDP port, sealed (level enc) with its key derived from the master
(the demo key by default), and shows the VERDICT the game sends back. The
path is the real one: gateway -> MQTT -> hub -> scavhunt -> hub -> gateway.

    python tools/scavbox.py --gateway 192.168.8.237 --box vault=0201 --box lab=0202

Then type commands, or pipe them in (each waits for its verdict):

    vault start text:RED      tap a token (the box remembers it)
    vault hint 1              ask for hint 1
    vault wrong               a wrong answer (no verdict)
    vault win                 the puzzle is solved
    vault end                 the team gives up
    vault data 32 a7080000    the box's own bytes on schema 32-255, for the hub
    wait 2                    pause, in seconds

Frame counters are kept in ~/.roclink-scavbox-<net>-<node>, so each run
continues where the last left off, as a real node must. Uses roclink-rfc's
reference sealing (tools/gen_vectors.py), from ../../protocol/roclink-rfc or --rfc.
"""

from __future__ import annotations

import argparse
import pathlib
import socket
import struct
import sys
import threading
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from scavhunt import messages as msg  # noqa: E402

ACK_REQ, MORE, DL_QUEUED = 0x01, 0x04, 0x08
DEMO_MASTER = bytes(range(16))
RESULTS = {
    0: "accepted", 1: "accepted, finished", 2: "already solved", 3: "locked", 4: "unknown team",
    5: "not running", 6: "go", 7: "busy", 8: "no session", 9: "hint granted", 10: "no more hints",
}
ARG = {3: "solve 0x{:04X} first", 6: "{} hints taken", 7: "{} s until it times out",
       9: "show hint {}", 10: "this box has {} hints"}
EXPECTS_VERDICT = {msg.SCHEMA_START, msg.SCHEMA_WIN, msg.SCHEMA_HINT}

rv = None  # roclink-rfc's gen_vectors, imported in main()
out_lock = threading.Lock()


def say(*args) -> None:
    with out_lock:
        print(*args, flush=True)


class Box:
    """One simulated box: its own socket, key, frame counter and token."""

    def __init__(self, name: str, net: int, node: int, master: bytes, gateway: tuple[str, int]):
        self.name, self.net, self.node, self.gateway = name, net, node, gateway
        self.key = rv.frame_key(rv.derive_node_key(master, net, node, 0), net, node, 0, 0)
        self.state = pathlib.Path.home() / f".roclink-scavbox-{net:02x}-{node:04x}"
        self.fcnt = int(self.state.read_text()) if self.state.exists() else 0
        self.sent: list[int] = []  # recent uplink counters: what a "now" downlink is bound to
        self.token: bytes | None = None  # the team whose session is open here
        self.result: int | None = None  # the last VERDICT's result
        self.lock = threading.Lock()
        self.acked = threading.Event()
        self.verdict = threading.Event()
        self.waiting_seq = -1
        self.dl_seen: dict[int, float] = {}
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("", 0))
        threading.Thread(target=self._receive, daemon=True).start()

    def _next_fcnt(self) -> int:
        with self.lock:
            self.fcnt += 1
            if self.fcnt >> 16:
                raise SystemExit(f"{self.name}: this simulator doesn't ratchet; delete {self.state}")
            self.state.write_text(str(self.fcnt))
            return self.fcnt

    def send(self, schema: int, payload: bytes) -> bool:
        """One DATA frame with ACK_REQ: retries the same bytes until the gateway ACKs."""
        fcnt = self._next_fcnt()
        seq = fcnt & 0xFF
        hdr = rv.header(rv.DATA, ACK_REQ | rv.SECURED, self.net, self.node, 0, seq, schema)
        frame = rv.seal(self.key, hdr, payload, fcnt, enc=True, tag_len=8, gen=0)
        self.sent = (self.sent + [fcnt])[-8:]
        self.waiting_seq = seq
        self.acked.clear()
        self.verdict.clear()
        for attempt in range(3):
            self.sock.sendto(frame, self.gateway)
            if self.acked.wait(1.0):
                return True
            say(f"[{self.name}] no ACK for fcnt {fcnt}{', retrying' if attempt < 2 else ''}")
        return False

    def _open(self, frame: bytes) -> tuple[bytes, int] | None:
        """Verify a gateway frame: bound to one of our recent counters (PROTOCOL.md 10.6)."""
        hdr, sec, rest = frame[:9], frame[9:14], frame[14:]
        tag_len = 8 if sec[0] & 0x10 else 4
        fcnt = struct.unpack("<I", sec[1:5])[0]
        masked = bytes([hdr[0], hdr[1] & ~0x70 & 0xFF]) + hdr[2:9]
        nonce = hdr[3:7] + bytes([hdr[2]]) + struct.pack("<I", fcnt) + b"\x00" * 4
        ccm = rv.AESCCM(self.key, tag_length=tag_len)
        for binding in reversed(self.sent):
            aad = masked + sec + struct.pack("<I", binding)
            try:
                if sec[0] & 0x20:
                    return ccm.decrypt(nonce, rest, aad), binding
                payload, tag = rest[:-tag_len], rest[-tag_len:]
                ccm.decrypt(nonce, tag, aad + payload)
                return payload, binding
            except Exception:
                continue
        return None

    def _receive(self) -> None:
        while True:
            frame, _ = self.sock.recvfrom(512)
            if len(frame) < 14 or not frame[1] & rv.SECURED:
                say(f"[{self.name}] dropped an unsealed or short frame ({len(frame)} bytes)")
                continue
            ftype, seq, schema = (frame[0] >> 2) & 0x07, frame[7], frame[8]
            gw = struct.unpack("<H", frame[3:5])[0]
            opened = self._open(frame)
            if opened is None:
                say(f"[{self.name}] a frame from {gw:04X} doesn't verify: dropped")
                continue
            data, _ = opened
            if ftype == rv.ACK:
                if seq == self.waiting_seq:
                    self.acked.set()
                continue
            if ftype != rv.DATA:
                continue
            if frame[1] & ACK_REQ:  # the node ACKs it, as a node frame with its next counter
                fcnt = self._next_fcnt()
                ack = rv.seal(self.key, rv.header(rv.ACK, rv.SECURED, self.net, self.node, gw, seq, 0),
                              b"", fcnt, enc=True, tag_len=8, gen=0)
                self.sock.sendto(ack, self.gateway)
            now = time.monotonic()
            if now - self.dl_seen.get(seq, -99) < 30:
                continue  # a re-offer we already took: ACKed again, not delivered twice
            self.dl_seen[seq] = now
            self._downlink(schema, data)

    def _downlink(self, schema: int, data: bytes) -> None:
        if schema == msg.SCHEMA_VERDICT and len(data) == 6 and data[0] == msg.VERSION:
            _, result, solved, required, arg = struct.unpack("<BBBBH", data)
            what = RESULTS.get(result, f"result {result}")
            extra = ARG[result].format(arg) if result in ARG else ""
            say(f"[{self.name}] VERDICT {what}{': ' + extra if extra else ''} "
                f"(solved {solved} of {required} required)")
            if result in (0, 1):  # solved: the box resets for the next team
                self.token = None
            self.result = result
            self.verdict.set()
        elif schema == msg.SCHEMA_CONTROL and len(data) >= 2:
            say(f"[{self.name}] CONTROL {({1: 'reset', 2: 'disable', 3: 'enable'}).get(data[1], data[1])}")
            if data[1] == msg.CONTROL_RESET:
                self.token = None
        else:
            say(f"[{self.name}] downlink schema {schema}: {data.hex()}")

    def command(self, words: list[str]) -> None:
        op = words[0]
        if op == "data":  # the box's own data, for the hub, not the game: no token needed
            if len(words) != 3 or not words[1].isdigit() or int(words[1]) not in msg.BOX_DATA:
                raise ValueError("data SCHEMA HEX (schema 32-255)")
            ok = self.send(int(words[1]), bytes.fromhex(words[2]))
            say(f"[{self.name}] DATA {'sent (fcnt %d), ACKed' % self.fcnt if ok else 'never ACKed'}")
            return
        tapped = None
        if op == "start":
            if len(words) < 2:
                raise ValueError("start TOKEN (text:RED or a UID in hex)")
            tapped = msg.parse_token(words[1])
        elif self.token is None:
            raise ValueError(f"{self.name}: no session: start TOKEN first")
        schema, payload = {
            "start": (msg.SCHEMA_START, lambda: msg.box_message(tapped)),
            "win": (msg.SCHEMA_WIN, lambda: msg.box_message(self.token)),
            "wrong": (msg.SCHEMA_WRONG, lambda: msg.box_message(self.token)),
            "end": (msg.SCHEMA_END, lambda: msg.box_message(self.token)),
            "hint": (msg.SCHEMA_HINT, lambda: msg.box_message(self.token, int(words[1]))),
        }.get(op, (None, None))
        if schema is None:
            raise ValueError(f"unknown command {op!r}")
        body = payload()
        if not self.send(schema, body):
            say(f"[{self.name}] {op.upper()}: the gateway never ACKed it")
            return
        say(f"[{self.name}] {op.upper()} sent (fcnt {self.fcnt}), ACKed")
        if schema in EXPECTS_VERDICT and not self.verdict.wait(10):
            say(f"[{self.name}] no VERDICT within 10 s")
        elif tapped is not None and self.result == 6:  # go: this team's session now
            self.token = tapped
        if op == "end":
            self.token = None


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--gateway", required=True)
    p.add_argument("--port", type=int, default=3333)
    p.add_argument("--net", type=lambda s: int(s, 0), default=0x42)
    p.add_argument("--box", action="append", required=True, metavar="NAME=NODE",
                   help="a box and its node address in hex, e.g. vault=0201; repeat for more")
    p.add_argument("--master", help="network master key, 32 hex digits (default: the demo key)")
    p.add_argument("--rfc", type=pathlib.Path,
                   default=pathlib.Path(__file__).resolve().parents[3] / "protocol" / "roclink-rfc",
                   help="the roclink-rfc repository (default: ../../protocol/roclink-rfc)")
    a = p.parse_args()

    global rv
    sys.path.insert(0, str(a.rfc / "tools"))
    import gen_vectors  # it imports AESCCM from cryptography, which _open() uses too
    rv = gen_vectors

    master = bytes.fromhex(a.master) if a.master else DEMO_MASTER
    gateway = (a.gateway, a.port)
    boxes = {}
    for spec in a.box:
        name, _, node = spec.partition("=")
        boxes[name] = Box(name, a.net, int(node, 16), master, gateway)
        say(f"[{name}] node {a.net:02X}/{int(node, 16):04X}, fcnt {boxes[name].fcnt}")

    interactive = sys.stdin.isatty()
    while True:
        if interactive:
            with out_lock:
                print("> ", end="", flush=True)
        line = sys.stdin.readline()
        if not line:
            break
        words = line.split("#", 1)[0].split()
        if not words:
            continue
        if not interactive:
            say(f"> {' '.join(words)}")
        try:
            if words[0] == "wait":
                time.sleep(float(words[1]) if len(words) > 1 else 1)
            elif words[0] in boxes and len(words) > 1:
                boxes[words[0]].command(words[1:])
            else:
                raise ValueError(f"which box? {', '.join(boxes)}")
        except (ValueError, msg.MessageError) as e:
            say(f"error: {e}")
    time.sleep(1)  # late downlinks
    return 0


if __name__ == "__main__":
    sys.exit(main())
