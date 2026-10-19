"""roclink envelope decoder (PROTOCOL.md §3 and Annex A).

The hub only decodes frames; it never builds them. A secured frame is split
into its fields but not verified, because the hub holds no keys. Secured
uplinks reach the hub as JSON forward records from a gateway that has already
verified and decrypted them (see record.py).
"""

from dataclasses import dataclass

VERSION = 1
HEADER_LEN = 9

TYPE_DATA = 0
TYPE_ACK = 1

FLAG_ACK_REQ = 0x01
FLAG_LISTEN = 0x02
FLAG_MORE = 0x04
FLAG_DL_QUEUED = 0x08
FLAG_RELAYED = 0x10
FLAG_SECURED = 0x80

ADDR_ANY_GATEWAY = 0x0000
NODE_MIN, NODE_MAX = 0x0001, 0xFEFF
GATEWAY_MIN, GATEWAY_MAX = 0xFF00, 0xFFFE


class FrameError(ValueError):
    """The bytes are not a frame a v1 receiver accepts."""


@dataclass(frozen=True, slots=True)
class Frame:
    type: int
    flags: int
    net_id: int
    src: int
    dst: int
    seq: int
    schema: int
    payload: bytes
    # Present only when SECURED is set (§10.2).
    sec_ctrl: int | None = None
    fcnt: int | None = None
    tag: bytes | None = None

    @property
    def secured(self) -> bool:
        return bool(self.flags & FLAG_SECURED)


def is_node(addr: int) -> bool:
    return NODE_MIN <= addr <= NODE_MAX


def is_gateway(addr: int) -> bool:
    return GATEWAY_MIN <= addr <= GATEWAY_MAX


def decode(b: bytes) -> Frame:
    """Decode one frame, following the Annex A checklist."""
    if len(b) < HEADER_LEN:
        raise FrameError(f"{len(b)} bytes is shorter than the 9-byte header")
    ver = b[0] >> 5
    if ver != VERSION:
        raise FrameError(f"version {ver}, expected {VERSION}")
    ftype = (b[0] >> 2) & 0x07
    if ftype not in (TYPE_DATA, TYPE_ACK):
        raise FrameError(f"reserved frame type {ftype}")

    flags, net_id = b[1], b[2]
    src = int.from_bytes(b[3:5], "little")
    dst = int.from_bytes(b[5:7], "little")
    seq, schema = b[7], b[8]

    if flags & FLAG_RELAYED:
        raise FrameError("relayed frames are reserved in v1")
    off = HEADER_LEN

    sec_ctrl = fcnt = tag = None
    end = len(b)
    if flags & FLAG_SECURED:
        if len(b) < off + 5:
            raise FrameError("too short for the security header")
        sec_ctrl = b[off]
        fcnt = int.from_bytes(b[off + 1 : off + 5], "little")
        off += 5
        taglen = 8 if sec_ctrl & 0x10 else 4
        if len(b) < off + taglen:
            raise FrameError("too short for the security tag")
        end = len(b) - taglen
        tag = bytes(b[end:])
    payload = bytes(b[off:end])

    if ftype == TYPE_ACK and (payload or schema != 0):
        raise FrameError("an ACK carries no payload and schema 0")

    return Frame(ftype, flags, net_id, src, dst, seq, schema, payload, sec_ctrl, fcnt, tag)
