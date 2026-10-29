#!/usr/bin/env python3
"""Decode what the rfgateway publishes for rfidprox nodes: a stand-in for the
hub while there isn't one. Reads `mosquitto_sub -v` output on stdin.

    mosquitto_sub -h localhost -u admin -P PASSWORD -t 'roclink/in/#' -v | python3 watch.py

Uplink records (GATEWAY_INTERFACE.md section 3) are decoded by schema
(docs/BACKEND.md section 1); drop reports and anything else are shown as they are.
No dependencies beyond Python 3.
"""
import json
import struct
import sys
import time

SCHEMAS = {0: "PING", 1: "HELLO", 2: "AUTH_REQ", 3: "RENEW", 4: "SESSION_END", 5: "STATUS"}
RADIOS = {1: "LoRa", 2: "Wi-Fi UDP", 3: "ESP-NOW", 4: "HaLow"}


def lp_string(b: bytes, i: int) -> str:
    n = b[i] if i < len(b) else 0
    return b[i + 1 : i + 1 + n].decode(errors="replace")


def decode(schema: int, p: bytes) -> str:
    if schema == 0:
        return "link check" if p[:1] == b"\x01" else f"control op {p[:1].hex()}"
    if not p or p[0] != 1:
        return f"payload {p.hex()} (not version 1)"
    try:
        if schema == 1:
            fw, radio = struct.unpack_from("<HB", p, 1)
            box = lp_string(p, 4)
            return f"firmware {fw:04X}, radio {RADIOS.get(radio, radio)}, box {box or '(none)'}"
        if schema == 2:
            req, method, atqa, sak, n = struct.unpack_from("<BBHBB", p, 1)
            uid = p[7 : 7 + n].hex().upper()
            kind = "EM4100 125 kHz" if atqa == 0 and sak == 0 else f"ATQA {atqa:04X}, SAK {sak:02X}"
            return f"req {req}, card UID {uid} ({n} bytes), {kind}, method {method}"
        if schema in (3, 4):
            session = struct.unpack_from("<H", p, 1)[0]
            extra = f", reason {p[3]}" if schema == 4 and len(p) > 3 else ""
            return f"session {session:04X}{extra}"
        if schema == 5:
            state, session, box, buffered = struct.unpack_from("<BHBB", p, 1)
            return f"state {state}, session {session:04X}, box {box}, buffered {buffered}"
        if schema >= 32:
            session = struct.unpack_from("<H", p, 0)[0]
            return f"box data, session {session:04X}: {p[2:].hex()}"
    except struct.error:
        pass
    return f"payload {p.hex()}"


def main() -> None:
    for line in sys.stdin:
        topic, _, body = line.strip().partition(" ")
        stamp = time.strftime("%H:%M:%S")
        try:
            rec = json.loads(body)
        except ValueError:
            print(f"{stamp} {topic} {body}", flush=True)
            continue
        if topic.endswith("/up"):
            schema = int(rec.get("schema", -1))
            name = SCHEMAS.get(schema, f"schema {schema}" if schema < 32 else "BOX")
            p = bytes.fromhex(rec.get("payload", ""))
            radio = rec.get("radio", {})
            rssi = f" rssi {radio['rssi']}" if "rssi" in radio else ""
            print(f"{stamp} {rec.get('net', 0):02X}/{rec.get('src')} seq {rec.get('seq')} "
                  f"fcnt {rec.get('fcnt', '-')} [{rec.get('sec')}] via {radio.get('transport', '?')}{rssi}: "
                  f"{name}: {decode(schema, p)}", flush=True)
        elif topic.endswith("/rejected"):
            print(f"{stamp} DROPPED by {rec.get('gateway')}: {rec.get('reason')} x{rec.get('count', 1)} "
                  f"claimed src {rec.get('src', '?')} fcnt {rec.get('fcnt', '-')}", flush=True)
        else:
            print(f"{stamp} {topic} {body}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
