#!/usr/bin/env python3
"""Generate roclink secured-frame test vectors independently of the C library.

Written from docs/PROTOCOL.md section 10 (not translated from src/), using the
`cryptography` package (OpenSSL) for AES-CCM and AES-CMAC. It also serves as a
small reference implementation of sealing for anyone writing their own client
or server. Prints the C arrays used by test/test_secure.c and the hex rows for
PROTOCOL.md section 11.

    pip install cryptography   # or your distro's python-cryptography
    python3 tools/gen_vectors.py
"""
import struct

from cryptography.hazmat.primitives import cmac
from cryptography.hazmat.primitives.ciphers import algorithms
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

SECURED, RELAYED, HOPS = 0x80, 0x10, 0x60
DATA, ACK = 0, 1


def aes_cmac(key: bytes, msg: bytes) -> bytes:
    c = cmac.CMAC(algorithms.AES(key))
    c.update(msg)
    return c.finalize()


def kdf(key: bytes, label: bytes, context: bytes) -> bytes:
    """NIST SP 800-108 counter mode, PRF = AES-CMAC, one 128-bit block."""
    return aes_cmac(key, b"\x01" + label + b"\x00" + context + b"\x00\x80")


def ctx(net: int, node: int, gen: int, epoch=None) -> bytes:
    c = bytes([net]) + struct.pack("<H", node) + bytes([gen])
    return c if epoch is None else c + struct.pack("<I", epoch)


def derive_node_key(master: bytes, net: int, node: int, gen: int) -> bytes:
    """CK(0), the node's provisioned chain key."""
    return kdf(master, b"roclink node key", ctx(net, node, gen))


def chain_step(ck: bytes, net: int, node: int, gen: int, to_epoch: int) -> bytes:
    """CK(e) -> CK(e+1); `to_epoch` is e+1."""
    return kdf(ck, b"roclink chain", ctx(net, node, gen, to_epoch))


def frame_key(ck: bytes, net: int, node: int, gen: int, epoch: int) -> bytes:
    """FK(e), the AES-CCM key for frames in epoch e."""
    return kdf(ck, b"roclink frame", ctx(net, node, gen, epoch))


def header(ftype, flags, net, src, dst, seq, schema) -> bytes:
    return bytes([(1 << 5) | (ftype << 2), flags, net]) + struct.pack("<HH", src, dst) + bytes([seq, schema])


def seal(key, hdr: bytes, payload: bytes, fcnt: int, enc: bool, tag_len: int, gen: int,
         binding=None) -> bytes:
    """Seal one frame. `hdr` is the 9-byte header with SECURED already set."""
    net, src_dst = hdr[2], hdr[3:7]
    sec = bytes([(0 << 6) | (0x20 if enc else 0) | (0x10 if tag_len == 8 else 0) | gen])
    sec += struct.pack("<I", fcnt)
    masked = bytes([hdr[0], hdr[1] & ~(RELAYED | HOPS) & 0xFF]) + hdr[2:9]
    aad = masked + sec + (struct.pack("<I", binding) if binding is not None else b"")
    nonce = src_dst + bytes([net]) + struct.pack("<I", fcnt) + b"\x00" * 4
    ccm = AESCCM(key, tag_length=tag_len)
    if enc:
        body = ccm.encrypt(nonce, payload, aad)  # ciphertext || tag
    else:
        body = payload + ccm.encrypt(nonce, b"", aad + payload)  # plain || tag
    return hdr + sec + body


def c_array(name, data: bytes) -> str:
    rows = [", ".join(f"0x{b:02X}" for b in data[i:i + 12]) for i in range(0, len(data), 12)]
    return f"static const uint8_t {name}[{len(data)}] = {{\n    " + ",\n    ".join(rows) + "};"


def hex_row(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


def self_check():
    # RFC 4493 section 4, examples 1 and 2.
    k = bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c")
    assert aes_cmac(k, b"") == bytes.fromhex("bb1d6929e95937287fa37d129b756746")
    assert aes_cmac(k, bytes.fromhex("6bc1bee22e409f96e93d7e117393172a")) == bytes.fromhex(
        "070a16b46b4d4144f79bdd9dd04a287c")
    # RFC 3610 packet vector #1 (13-byte nonce, 8-byte tag, 8 bytes of AAD).
    key = bytes(range(0xC0, 0xD0))
    nonce = bytes.fromhex("00000003020100a0a1a2a3a4a5")
    pkt = bytes(range(0x00, 0x1F))
    out = AESCCM(key, tag_length=8).encrypt(nonce, pkt[8:], pkt[:8])
    assert pkt[:8] + out == bytes.fromhex(
        "0001020304050607588c979a61c663d2f066d0c2c0f989806d5f6b61dac38417e8d12cfdf926e0")


def main():
    self_check()
    master = bytes(range(16))
    net, node, gw, gen = 0x42, 0x1234, 0xFF01, 0
    node_key = derive_node_key(master, net, node, gen)       # CK(0)
    fk0 = frame_key(node_key, net, node, gen, 0)
    ck1 = chain_step(node_key, net, node, gen, 1)
    fk1 = frame_key(ck1, net, node, gen, 1)

    bme = bytes.fromhex("01 2909 1810 CD8B0100 C0510200".replace(" ", ""))
    v = {}
    v["S1"] = seal(fk0, header(DATA, 0x03 | SECURED, net, node, 0x0000, 7, 0x20), bme,
                   fcnt=1, enc=True, tag_len=8, gen=gen)
    v["S2"] = seal(fk0, header(DATA, SECURED, net, node, gw, 8, 0x01), b"hi",
                   fcnt=2, enc=False, tag_len=4, gen=gen)
    v["S3"] = seal(fk0, header(ACK, 0x04 | SECURED, net, gw, node, 7, 0x00), b"",
                   fcnt=0x100, enc=True, tag_len=8, gen=gen, binding=1)
    v["S4"] = seal(fk0, header(DATA, 0x09 | SECURED, net, gw, node, 0x90, 0x0A), b"\x3c\x00",
                   fcnt=0x101, enc=True, tag_len=8, gen=gen, binding=1)
    # S6: the first frame of epoch 1 (fcnt 0x00010000), under FK(1)
    v["S6"] = seal(fk1, header(DATA, 0x02 | SECURED, net, node, 0x0000, 9, 0x20), bme,
                   fcnt=0x00010000, enc=True, tag_len=8, gen=gen)

    print("/* Generated by tools/gen_vectors.py; do not edit by hand. */")
    print(c_array("MASTER_KEY", master))
    print(c_array("NODE_KEY", node_key))
    print(c_array("FRAME_KEY_0", fk0))
    print(c_array("CHAIN_KEY_1", ck1))
    print(c_array("FRAME_KEY_1", fk1))
    for name, frame in v.items():
        print(c_array("V" + name, frame))
    print()
    print("node_key (CK0):", hex_row(node_key))
    print("FK0:", hex_row(fk0))
    print("CK1:", hex_row(ck1))
    print("FK1:", hex_row(fk1))
    for name, frame in v.items():
        print(f"{name}: {hex_row(frame)}")


if __name__ == "__main__":
    main()
