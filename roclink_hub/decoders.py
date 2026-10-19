"""Application payload decoders, chosen per schema in the config's [schemas] table.

roclink never looks inside a payload: what a schema's bytes mean is the
deployment's choice (PROTOCOL.md §5). A decoder takes the payload bytes and
returns a flat dict. Every int or float in that dict is also stored as a
time-series reading. A decoder raises ValueError when the bytes don't fit.

To add a format, write a function here and add it to DECODERS.
"""

import struct


def _need(payload: bytes, size: int, name: str) -> None:
    if len(payload) != size:
        raise ValueError(f"{name} payload is {size} bytes, got {len(payload)}")


def env(payload: bytes) -> dict:
    """examples/c ENV: int16 temp_cC, uint16 rh_cPct, uint16 batt_mV."""
    _need(payload, 6, "env")
    temp_cc, rh_cpct, batt_mv = struct.unpack("<hHH", payload)
    return {"temp_c": temp_cc / 100, "rh_pct": rh_cpct / 100, "batt_v": batt_mv / 1000}


def event(payload: bytes) -> dict:
    """examples/c EVENT: uint8 event, uint32 count."""
    _need(payload, 5, "event")
    code, count = struct.unpack("<BI", payload)
    return {"event": code, "count": count}


def text(payload: bytes) -> dict:
    """UTF-8 text, such as the Arduino sketches' messages."""
    return {"text": payload.decode("utf-8")}


DECODERS = {
    "env": env,
    "event": event,
    "text": text,
}
