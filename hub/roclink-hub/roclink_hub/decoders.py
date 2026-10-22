"""Application payload decoders, chosen per schema in the config's [schemas] table.

roclink never looks inside a payload: what a schema's bytes mean is the
deployment's choice (PROTOCOL.md §5). A decoder takes the payload bytes and
returns a flat dict. Every int or float in that dict is also stored as a
time-series reading. A decoder raises ValueError when the bytes don't fit.

To add a format, write a function here and add it to DECODERS, or, for a
fixed binary layout, declare it in the config ([decoder.<name>], Layout).
"""

import struct
from dataclasses import dataclass


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


@dataclass(frozen=True)
class Layout:
    """A decoder declared in the config instead of written here: a device's
    message as a fixed binary layout ([decoder.<name>] in config.example.toml).

    `fmt` is a Python struct format with an explicit byte order ("<" or ">"),
    one entry per field in `fields`. `scale` multiplies a field's value; `bits`
    splits an integer field into named one-bit fields, lowest bit first."""

    name: str
    fmt: str
    fields: tuple[str, ...]
    scale: tuple[tuple[str, float], ...] = ()
    bits: tuple[tuple[str, tuple[str, ...]], ...] = ()

    def __call__(self, payload: bytes) -> dict:
        _need(payload, struct.calcsize(self.fmt), self.name)
        out = {}
        for name, value in zip(self.fields, struct.unpack(self.fmt, payload)):
            if isinstance(value, bytes):  # "s": text, padded with NULs
                value = value.rstrip(b"\x00").decode("utf-8", errors="replace")
            out[name] = value
        for name, factor in self.scale:
            out[name] = out[name] * factor
        for name, flags in self.bits:
            value = out.pop(name)
            for i, flag in enumerate(flags):
                if flag:
                    out[flag] = (value >> i) & 1
        return out


def layout(name: str, values: dict) -> Layout:
    """A Layout from its config table. Raises ValueError on a bad one."""
    unknown = set(values) - {"struct", "fields", "scale", "bits"}
    if unknown:
        raise ValueError(f"unknown settings {', '.join(sorted(unknown))}")
    fmt, names = values.get("struct"), values.get("fields")
    if not isinstance(fmt, str) or fmt[:1] not in "<>":
        raise ValueError('struct must be a struct format starting with "<" or ">"')
    try:
        count = len(struct.unpack(fmt, bytes(struct.calcsize(fmt))))
    except struct.error as e:
        raise ValueError(f"struct {fmt!r}: {e}") from None
    if not isinstance(names, list) or not all(isinstance(n, str) and n for n in names):
        raise ValueError("fields must be a list of names")
    if len(names) != count or len(set(names)) != count:
        raise ValueError(f"struct {fmt!r} has {count} fields: give each a different name")
    scale, bits = values.get("scale", {}), values.get("bits", {})
    for table, what in ((scale, "scale"), (bits, "bits")):
        if not isinstance(table, dict) or set(table) - set(names):
            raise ValueError(f"{what} must be a table of field names")
    if not all(isinstance(f, (int, float)) and not isinstance(f, bool) for f in scale.values()):
        raise ValueError("scale values must be numbers")
    if not all(isinstance(f, list) and all(isinstance(n, str) for n in f) for f in bits.values()):
        raise ValueError("bits values must be lists of names (\"\" skips a bit)")
    return Layout(name, fmt, tuple(names), tuple(scale.items()),
                  tuple((k, tuple(v)) for k, v in bits.items()))
