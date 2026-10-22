"""Turn one MQTT message into a Message, the unit the ingest process hands to
the writer.

Gateways publish to `roclink/in/<net>/<gateway>/up` (hex ids, for example
`roclink/in/42/ff01/up`). The MQTT payload is one of:

1. A JSON forward record (preferred). The gateway has already checked the
   envelope and, for secured frames, verified and decrypted it:

       {"v": 1, "gateway": "0xFF01", "time": 1791460800123,
        "net": 66, "src": "0x1234", "dst": 0, "seq": 7, "schema": 1, "flags": 3,
        "sec": "enc", "fcnt": 1, "key_gen": 0,
        "payload": "29091810cd8b",
        "radio": {"transport": "lora", "rssi": -87, "snr": 7.5}}

   Ids may be JSON numbers or "0x..." strings. Required: net, src, seq, schema,
   payload. `fcnt` and `key_gen` are required when `sec` is "auth" or "enc".
   `gateway` defaults to the one in the topic; `time` is the gateway's receive
   time in unix ms.

2. A raw roclink frame, for bench bridges that only move bytes. It must be
   unsecured, because the hub holds no keys to verify it.

A JSON record starts with "{" (0x7B), which as a frame would be version 3, so
the two never get confused.

Parsing never raises. A message that can't be parsed carries an error instead
of an uplink, and the writer still records it.
"""

import json
import re
from dataclasses import dataclass

from . import frame
from .decoders import DECODERS

SECURITY_LEVELS = ("none", "auth", "enc")


class RecordError(ValueError):
    pass


@dataclass(slots=True)
class Uplink:
    # Envelope
    net_id: int
    node_id: int
    dst: int
    seq: int
    schema: int
    flags: int
    security: str  # "none", "auth" or "enc"
    fcnt: int | None
    key_gen: int | None
    payload: bytes
    # This reception: which gateway heard it, and how well
    gateway_id: int | None = None
    gateway_time: int | None = None
    transport: str | None = None
    rssi: float | None = None
    snr: float | None = None
    # Application payload, decoded by the decoder for its device type and schema
    device_type: str | None = None
    decoder: str | None = None
    decoded: dict | None = None
    decode_error: str | None = None


@dataclass(slots=True)
class Message:
    received_at: int  # unix ms, hub clock
    topic: str
    raw: bytes
    uplink: Uplink | None
    error: str | None = None


@dataclass(slots=True)
class RejectionEvent:
    """A frame or message that was dropped: reported by a gateway (PROTOCOL.md
    GATEWAY_INTERFACE.md §4), or dropped by the hub itself. Every field but `reason` came
    from the dropped frame, so its sender chose it: `claimed_src` is only what
    the frame claims."""

    reason: str
    source: str  # "gateway" or "hub"
    gateway_id: int | None = None
    net_id: int | None = None
    claimed_src: int | None = None
    count: int = 1
    sample: bytes | None = None  # the frame (gateway) or the message (hub), cut to SAMPLE_MAX
    rssi: float | None = None
    detail: str | None = None  # the hub's own reason, for its drops


SAMPLE_MAX = 256
REASON = re.compile(r"[a-z_]{1,32}")


def is_rejection_topic(topic: str) -> bool:
    return topic.rsplit("/", 1)[-1] == "rejected"


def parse_rejection(topic: str, raw: bytes) -> RejectionEvent:
    """A gateway's rejection report. Raises RecordError if it isn't one."""
    if raw[:1] != b"{":
        raise RecordError("a rejection report must be a JSON object")
    try:
        rec = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise RecordError(f"bad JSON: {e}") from None
    if rec.get("v", 1) != 1:
        raise RecordError(f"unsupported record version {rec.get('v')!r}")
    if rec.get("kind") != "rejected":
        raise RecordError('kind must be "rejected"')
    reason = rec.get("reason")
    if not isinstance(reason, str) or not REASON.fullmatch(reason):
        raise RecordError("reason must be 1-32 lowercase letters or _")
    radio = rec.get("radio") or {}
    if not isinstance(radio, dict):
        raise RecordError("radio must be an object")
    sample = None
    if rec.get("frame") is not None:
        if not isinstance(rec["frame"], str):
            raise RecordError("frame must be a hex string")
        try:
            sample = bytes.fromhex(rec["frame"])[:SAMPLE_MAX]
        except ValueError:
            raise RecordError("frame is not valid hex") from None
    return RejectionEvent(
        reason=reason,
        source="gateway",
        gateway_id=_int(
            rec, "gateway", frame.GATEWAY_MIN, frame.GATEWAY_MAX, default=gateway_from_topic(topic)
        ),
        net_id=_int(rec, "net", 0, 0xFF, default=None),
        claimed_src=_int(rec, "src", 0, 0xFFFF, default=None),  # any address: it's a claim
        count=_int(rec, "count", 1, 1_000_000, default=1),
        sample=sample,
        rssi=_num(radio, "rssi"),
    )


def parse_message(topic: str, raw: bytes, received_at: int, decoders) -> Message:
    """`decoders` is the config's Registry (device types), or a plain
    {schema: decoder} table, as for nodes with no device type."""
    try:
        uplink = parse_uplink(topic, raw)
    except (RecordError, frame.FrameError) as e:
        return Message(received_at, topic, raw, None, str(e))
    decode_payload(uplink, decoders)
    return Message(received_at, topic, raw, uplink)


def parse_uplink(topic: str, raw: bytes) -> Uplink:
    topic_gateway = gateway_from_topic(topic)
    if raw[:1] == b"{":
        try:
            record = json.loads(raw)
        except (UnicodeDecodeError, json.JSONDecodeError) as e:
            raise RecordError(f"bad JSON: {e}") from None
        return _from_record(record, topic_gateway)
    return _from_frame(frame.decode(raw), topic_gateway)


def to_record(uplink: Uplink, received_at: int) -> dict:
    """The forward record the router publishes to services and the public topic.

    It's the v1 record above, plus the hub's receive time (`hub_time`) and the
    decoder's output, so a service can read the same parser's input.
    """
    rec = {
        "v": 1,
        "net": uplink.net_id,
        "src": uplink.node_id,
        "dst": uplink.dst,
        "seq": uplink.seq,
        "schema": uplink.schema,
        "flags": uplink.flags,
        "sec": uplink.security,
        "payload": uplink.payload.hex(),
        "hub_time": received_at,
    }
    if uplink.fcnt is not None:
        rec["fcnt"] = uplink.fcnt
        rec["key_gen"] = uplink.key_gen
    if uplink.gateway_id is not None:
        rec["gateway"] = uplink.gateway_id
    if uplink.gateway_time is not None:
        rec["time"] = uplink.gateway_time
    radio = {"transport": uplink.transport, "rssi": uplink.rssi, "snr": uplink.snr}
    radio = {k: v for k, v in radio.items() if v is not None}
    if radio:
        rec["radio"] = radio
    if uplink.device_type is not None:
        rec["device_type"] = uplink.device_type
    if uplink.decoder is not None:
        rec["decoder"] = uplink.decoder
        rec["decoded"] = uplink.decoded
        if uplink.decode_error:
            rec["decode_error"] = uplink.decode_error
    return rec


def decode_payload(uplink: Uplink, decoders) -> None:
    """Fill in the device type and decoded fields. A schema with no decoder is
    left undecoded. Schemas belong to device types (PROTOCOL.md §5)."""
    if hasattr(decoders, "decoder_for"):
        t = decoders.device_type(uplink.net_id, uplink.node_id)
        uplink.device_type = t.name if t is not None else None
        name = decoders.decoder_for(uplink.net_id, uplink.node_id, uplink.schema)
    else:
        name = decoders.get(uplink.schema)
    if name is None:
        return
    uplink.decoder = name
    try:
        if hasattr(decoders, "decode"):
            uplink.decoded = decoders.decode(name, uplink.payload)
        else:
            uplink.decoded = DECODERS[name](uplink.payload)
    except ValueError as e:
        uplink.decode_error = str(e)


def gateway_from_topic(topic: str) -> int | None:
    """The <gateway> in `.../<net>/<gateway>/up`, if it's a gateway address."""
    parts = topic.split("/")
    if len(parts) < 3:
        return None
    try:
        addr = int(parts[-2], 16)
    except ValueError:
        return None
    return addr if frame.is_gateway(addr) else None


def _from_frame(f: frame.Frame, gateway_id: int | None) -> Uplink:
    if f.type != frame.TYPE_DATA:
        raise RecordError("not a DATA frame")
    if f.secured:
        raise RecordError("raw secured frame: the hub has no keys, send it via a gateway")
    if not frame.is_node(f.src):
        raise RecordError(f"src 0x{f.src:04X} is not a node address")
    return Uplink(
        net_id=f.net_id,
        node_id=f.src,
        dst=f.dst,
        seq=f.seq,
        schema=f.schema,
        flags=f.flags,
        security="none",
        fcnt=None,
        key_gen=None,
        payload=f.payload,
        gateway_id=gateway_id,
    )


def _from_record(rec: dict, topic_gateway: int | None) -> Uplink:
    version = rec.get("v", 1)
    if version != 1:
        raise RecordError(f"unsupported record version {version!r}")
    security = rec.get("sec", "none")
    if security not in SECURITY_LEVELS:
        raise RecordError(f"sec must be one of {SECURITY_LEVELS}, got {security!r}")
    secured = security != "none"
    radio = rec.get("radio") or {}
    if not isinstance(radio, dict):
        raise RecordError("radio must be an object")
    payload = rec.get("payload")
    if not isinstance(payload, str):
        raise RecordError("payload must be a hex string")
    try:
        payload = bytes.fromhex(payload)
    except ValueError:
        raise RecordError("payload is not valid hex") from None

    return Uplink(
        net_id=_int(rec, "net", 0, 0xFF),
        node_id=_int(rec, "src", frame.NODE_MIN, frame.NODE_MAX),
        dst=_int(rec, "dst", 0, 0xFFFF, default=frame.ADDR_ANY_GATEWAY),
        seq=_int(rec, "seq", 0, 0xFF),
        schema=_int(rec, "schema", 0, 0xFF),
        flags=_int(rec, "flags", 0, 0xFF, default=0),
        security=security,
        fcnt=_int(rec, "fcnt", 0, 0xFFFFFFFF) if secured else None,
        key_gen=_int(rec, "key_gen", 0, 15) if secured else None,
        payload=payload,
        gateway_id=_int(rec, "gateway", frame.GATEWAY_MIN, frame.GATEWAY_MAX, default=topic_gateway),
        gateway_time=_int(rec, "time", 0, 2**63 - 1, default=None),
        transport=_str(radio, "transport"),
        rssi=_num(radio, "rssi"),
        snr=_num(radio, "snr"),
    )


_REQUIRED = object()


def _int(rec: dict, key: str, lo: int, hi: int, default=_REQUIRED) -> int | None:
    value = rec.get(key)
    if value is None:
        if default is _REQUIRED:
            raise RecordError(f"missing {key}")
        return default
    if isinstance(value, str):
        try:
            value = int(value, 0)
        except ValueError:
            raise RecordError(f"{key} is not an integer: {value!r}") from None
    if isinstance(value, bool) or not isinstance(value, int):
        raise RecordError(f"{key} is not an integer: {value!r}")
    if not lo <= value <= hi:
        raise RecordError(f"{key} {value} is outside {lo}..{hi}")
    return value


def _num(rec: dict, key: str) -> float | None:
    value = rec.get(key)
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise RecordError(f"{key} is not a number: {value!r}")
    return float(value)


def _str(rec: dict, key: str) -> str | None:
    value = rec.get(key)
    if value is not None and not isinstance(value, str):
        raise RecordError(f"{key} is not a string: {value!r}")
    return value
