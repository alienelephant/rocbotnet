#!/usr/bin/env python3
"""A roclink test node over UDP: the Wi-Fi path, end to end.

Sends one DATA frame with ACK_REQ to the gateway and checks its ACK. On a
secured net the frame is sealed (level enc) with the node's key derived from
the master (the demo key by default), and the gateway's ACK must verify,
bound to this frame's counter (PROTOCOL.md 10.6).

    python3 tools/test_node.py --gateway 192.168.8.237            # net 0x42, secured
    python3 tools/test_node.py --gateway 192.168.8.237 --open     # net 0xF0, unsecured

The frame counter is kept in ~/.roclink-test-node-<net>-<node> so each run
sends a new one, as a real node must. Uses roclink-rfc's reference sealing
(tools/gen_vectors.py).
"""
import argparse
import os
import pathlib
import socket
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "roclink-rfc" / "tools"))
from cryptography.hazmat.primitives.ciphers.aead import AESCCM  # noqa: E402
from gen_vectors import ACK, DATA, SECURED, derive_node_key, frame_key, header, seal  # noqa: E402

DEMO_MASTER = bytes(range(16))
ACK_REQ, MORE, DL_QUEUED = 0x01, 0x04, 0x08


def open_reply(key, frame, binding):
    """Verify a gateway frame sealed with the node's frame key, bound to our fcnt."""
    hdr, sec, rest = frame[:9], frame[9:14], frame[14:]
    tag_len = 8 if sec[0] & 0x10 else 4
    fcnt = struct.unpack("<I", sec[1:5])[0]
    masked = bytes([hdr[0], hdr[1] & ~0x70 & 0xFF]) + hdr[2:9]
    aad = masked + sec + struct.pack("<I", binding)
    nonce = hdr[3:7] + bytes([hdr[2]]) + struct.pack("<I", fcnt) + b"\x00" * 4
    ccm = AESCCM(key, tag_length=tag_len)
    if sec[0] & 0x20:
        return ccm.decrypt(nonce, rest, aad)
    payload, tag = rest[:-tag_len], rest[-tag_len:]
    ccm.decrypt(nonce, tag, aad + payload)
    return payload


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--gateway", required=True)
    p.add_argument("--port", type=int, default=3333)
    p.add_argument("--node", type=lambda s: int(s, 16), default=0x7E57,
                   help="node address in hex (default 7E57; not one a real node uses, or its counter wins)")
    p.add_argument("--open", action="store_true", help="unsecured, on the open test net 0xF0")
    p.add_argument("--net", type=lambda s: int(s, 0))
    p.add_argument("--master", help="network master key, 32 hex digits (default: the demo key)")
    p.add_argument("--schema", type=int, default=1)
    p.add_argument("--text", default="hello from the test node")
    a = p.parse_args()
    net = a.net if a.net is not None else (0xF0 if a.open else 0x42)

    state = pathlib.Path.home() / f".roclink-test-node-{net:02x}-{a.node:04x}"
    fcnt = int(state.read_text()) + 1 if state.exists() else 1
    state.write_text(str(fcnt))
    seq = fcnt & 0xFF
    payload = a.text.encode()

    if a.open:
        frame = header(DATA, ACK_REQ, net, a.node, 0, seq, a.schema) + payload
        key = None
    else:
        master = bytes.fromhex(a.master) if a.master else DEMO_MASTER
        ck = derive_node_key(master, net, a.node, 0)
        key = frame_key(ck, net, a.node, 0, fcnt >> 16)
        assert fcnt >> 16 == 0, "this test node doesn't ratchet; delete the state file"
        hdr = header(DATA, ACK_REQ | SECURED, net, a.node, 0, seq, a.schema)
        frame = seal(key, hdr, payload, fcnt, enc=True, tag_len=8, gen=0)

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("", 0))
    s.settimeout(3)
    s.sendto(frame, (a.gateway, a.port))
    print(f"sent net 0x{net:02X} node {a.node:04X} seq {seq} fcnt {fcnt}: {len(frame)} bytes")
    try:
        reply, _ = s.recvfrom(512)
    except socket.timeout:
        print("FAIL: no ACK within 3 s")
        return 1
    ftype = (reply[0] >> 2) & 0x07
    if ftype != ACK or reply[7] != seq:
        print(f"FAIL: expected an ACK for seq {seq}, got type {ftype} seq {reply[7]}")
        return 1
    src = struct.unpack("<H", reply[3:5])[0]
    if key is not None:
        if not reply[1] & SECURED:
            print("FAIL: the ACK isn't sealed")
            return 1
        try:
            open_reply(key, reply, fcnt)
        except Exception:
            print("FAIL: the ACK doesn't verify")
            return 1
        print(f"OK: sealed ACK from gateway {src:04X} verifies, bound to fcnt {fcnt}")
    else:
        print(f"OK: ACK from gateway {src:04X}")

    # MORE: a downlink follows in this reply window.
    while reply[1] & MORE:
        try:
            reply, _ = s.recvfrom(512)
        except socket.timeout:
            print("FAIL: the ACK said MORE, but nothing followed")
            return 1
        dl_seq, schema = reply[7], reply[8]
        if key is not None:
            try:
                data = open_reply(key, reply, fcnt)
            except Exception:
                print("FAIL: the downlink doesn't verify")
                return 1
        else:
            data = reply[9:]
        print(f"OK: downlink seq {dl_seq} schema {schema}: {data.hex()}"
              f"{' (more queued)' if reply[1] & DL_QUEUED else ''}")
        if reply[1] & ACK_REQ:  # the node ACKs it, as a node frame with its next counter
            fcnt += 1
            state.write_text(str(fcnt))
            if key is None:
                ack = header(ACK, 0, net, a.node, src, dl_seq, 0)
            else:
                ack = seal(key, header(ACK, SECURED, net, a.node, src, dl_seq, 0), b"", fcnt,
                           enc=True, tag_len=8, gen=0)
            s.sendto(ack, (a.gateway, a.port))
            print(f"   ACKed it (fcnt {fcnt})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
