"""The game's messages on the roclink wire (docs/GAME.md §6). All integers
are little-endian; the roclink header around them is the hub's business."""

from __future__ import annotations

import struct

SCHEMA_WIN = 1
SCHEMA_START = 2
SCHEMA_WRONG = 3
SCHEMA_HINT = 4
SCHEMA_END = 5
UPLINKS = (SCHEMA_WIN, SCHEMA_START, SCHEMA_WRONG, SCHEMA_HINT, SCHEMA_END)
SCHEMA_VERDICT = 10
SCHEMA_CONTROL = 11
VERSION = 1
TOKEN_MAX = 16

CONTROL_RESET, CONTROL_DISABLE, CONTROL_ENABLE = 1, 2, 3


class MessageError(ValueError):
    pass


def parse_box_message(schema: int, payload: bytes) -> tuple[bytes, int | None]:
    """An uplink: version | token length | token, and for HINT the hint
    number. Returns (token, hint number or None)."""
    if len(payload) < 2:
        raise MessageError("too short")
    if payload[0] != VERSION:
        raise MessageError(f"unknown version {payload[0]}")
    n = payload[1]
    extra = 1 if schema == SCHEMA_HINT else 0
    if not 1 <= n <= TOKEN_MAX or len(payload) != 2 + n + extra:
        raise MessageError(f"bad token length {n} in a {len(payload)}-byte payload")
    return payload[2 : 2 + n], (payload[2 + n] if extra else None)


def box_message(token: bytes, hint: int | None = None) -> bytes:
    """An uplink payload as a box sends it (for tests and box firmware)."""
    if not 1 <= len(token) <= TOKEN_MAX:
        raise MessageError("tokens are 1-16 bytes")
    return bytes([VERSION, len(token)]) + token + (bytes([hint]) if hint is not None else b"")


def verdict(result: int, solved: int, required: int, arg: int = 0) -> bytes:
    """VERDICT: version | result | solved | required | arg (§6: depends on the result)."""
    return struct.pack("<BBBBH", VERSION, result, min(solved, 255), min(required, 255), arg)


def control(command: int) -> bytes:
    return bytes([VERSION, command])


def parse_token(text: str) -> bytes:
    """A token as an operator writes it: hex for a card's UID ("04a1b2c3"), or
    "text:" and a typed code ("text:RED7")."""
    if text.startswith("text:"):
        token = text[5:].encode()
    else:
        try:
            token = bytes.fromhex(text.replace(":", "").replace(" ", ""))
        except ValueError:
            raise MessageError(f"{text!r} isn't hex; write typed codes as text:CODE") from None
    if not 1 <= len(token) <= TOKEN_MAX:
        raise MessageError("tokens are 1-16 bytes")
    return token
