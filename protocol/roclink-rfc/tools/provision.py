#!/usr/bin/env python3
"""Make the keys a roclink deployment needs from the network master key, a
secret that never leaves this machine (docs/SECURITY.md, deployment
requirements R1-R3). Each node's chain key CK(0) comes from it.

    python3 tools/provision.py --new 1234 1235             # make a master, print it once
    python3 tools/provision.py --master HEX 1234 1235
    python3 tools/provision.py --demo 1234                 # the published test-vector key

For each node it prints its CK(0) as a C array, NODE_<addr>_CHAIN_KEY:
provision it into the node (encrypted NVS), and give it to an enrolled-mode
gateway for roclink_sec_peer_enroll(). Pasted into a sketch's git-ignored
secrets.h, it replaces the demo key (examples/arduino/*_secure_*).
"""
import argparse
import secrets
import sys

from gen_vectors import derive_node_key

DEMO_MASTER = bytes(range(0x00, 0x10))


def c_array(name, b):
    return f"static const uint8_t {name}[16] = {{" + ", ".join(f"0x{x:02X}" for x in b) + "};"


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("nodes", nargs="*", help="node addresses in hex, e.g. 1234")
    p.add_argument("--net", default="42", help="network id in hex (default 42)")
    p.add_argument("--master", help="network master key, 32 hex digits")
    p.add_argument("--key-gen", type=int, default=0, help="roclink master key generation (0-15)")
    p.add_argument("--new", action="store_true", help="generate a new master key")
    p.add_argument("--demo", action="store_true", help="use the published test-vector key (never deploy it)")
    a = p.parse_args()

    if a.new:
        master = secrets.token_bytes(16)
        print("# New master key: store it offline. It is not written anywhere.")
        print(f"master {master.hex()}\n")
    elif a.demo:
        master = DEMO_MASTER
        print("# DEMO key from the test vectors: anyone can derive it. Never deploy it.\n")
    elif a.master:
        master = bytes.fromhex(a.master)
    else:
        sys.exit("give --new, --demo or --master")
    if len(master) != 16:
        sys.exit("the master key is 16 bytes (32 hex digits)")

    net = int(a.net, 16)
    for n in a.nodes:
        node = int(n, 16)
        if not 0x0001 <= node <= 0xFEFF:
            sys.exit(f"{n}: node addresses are 0001-FEFF")
        ck0 = derive_node_key(master, net, node, a.key_gen)
        print(f"# node {node:04X} (net {net:02X}, key_gen {a.key_gen})")
        print(c_array(f"NODE_{node:04X}_CHAIN_KEY", ck0) + "   // the node, and an enrolled gateway\n")


if __name__ == "__main__":
    main()
