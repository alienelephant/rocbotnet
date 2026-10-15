"""The game's messages on the roclink wire (docs/GAME.md §6). All integers
are little-endian; the roclink header around them is the hub's business."""

from __future__ import annotations

import struct

SCHEMA_WIN = 1
SCHEMA_ATTEMPT = 2
SCHEMA_VERDICT = 10
SCHEMA_CONTROL = 11
VERSION = 1
TOKEN_MAX = 16

CONTROL_RESET, CONTROL_DISABLE, CONTROL_ENABLE = 1, 2, 3


class MessageError(ValueError):
    pass


def parse_token_message(payload: bytes) -> bytes:
    """WIN and ATTEMPT: version | token length | token. Returns the token."""
    if len(payload) < 2:
        raise MessageError("too short")
    if payload[0] != VERSION:
        raise MessageError(f"unknown version {payload[0]}")
    n = payload[1]
    if not 1 <= n <= TOKEN_MAX or len(payload) != 2 + n:
        raise MessageError(f"bad token length {n} in a {len(payload)}-byte payload")
    return payload[2 : 2 + n]


def token_message(token: bytes) -> bytes:
    """The WIN or ATTEMPT payload a box sends (for tests and box firmware)."""
    if not 1 <= len(token) <= TOKEN_MAX:
        raise MessageError("tokens are 1-16 bytes")
    return bytes([VERSION, len(token)]) + token


def verdict(result: int, solved: int, required: int, needs_node: int = 0) -> bytes:
    """VERDICT: version | result | solved | required | needs (node address)."""
    return struct.pack("<BBBBH", VERSION, result, min(solved, 255), min(required, 255), needs_node)


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
