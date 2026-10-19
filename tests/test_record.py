import json
import struct

import pytest

from roclink_hub.record import gateway_from_topic, parse_message, parse_uplink

PORTS = {1: "env", 2: "event", 3: "text"}
TOPIC = "roclink/in/42/ff01/up"


def frame(schema: int, payload: bytes, seq: int = 7, src: int = 0x1234) -> bytes:
    return bytes([0x20, 0x00, 0x42]) + struct.pack("<HHBB", src, 0, seq, schema) + payload


def record(**fields) -> bytes:
    rec = {"net": 0x42, "src": "0x1234", "seq": 7, "schema": 1, "payload": "29090010d00b"}
    rec.update(fields)
    return json.dumps(rec).encode()


def parse(raw: bytes, topic: str = TOPIC):
    return parse_message(topic, raw, 1000, PORTS)


def test_raw_frame_takes_gateway_from_topic():
    msg = parse(frame(3, b"hello"))
    up = msg.uplink
    assert msg.error is None
    assert (up.net_id, up.node_id, up.seq, up.schema, up.security) == (0x42, 0x1234, 7, 3, "none")
    assert up.gateway_id == 0xFF01
    assert up.decoded == {"text": "hello"}


def test_env_decoder():
    up = parse(frame(1, struct.pack("<hHH", -512, 4567, 3012))).uplink
    assert up.decoder == "env"
    assert up.decoded == {"temp_c": -5.12, "rh_pct": 45.67, "batt_v": 3.012}


def test_decode_error_keeps_the_uplink():
    msg = parse(frame(1, b"Seconds since boot: 5"))
    assert msg.uplink is not None
    assert msg.uplink.decoded is None
    assert "env payload is 6 bytes" in msg.uplink.decode_error


def test_unknown_port_is_left_undecoded():
    up = parse(frame(99, b"\x01\x02")).uplink
    assert (up.decoder, up.decoded, up.decode_error) == (None, None, None)


def test_json_record_secured_with_radio():
    msg = parse(
        record(
            sec="enc",
            fcnt=1,
            key_gen=0,
            gateway="0xFF02",
            time=1791460800123,
            radio={"transport": "lora", "rssi": -87, "snr": 7.5},
        )
    )
    up = msg.uplink
    assert msg.error is None
    assert (up.security, up.fcnt, up.key_gen) == ("enc", 1, 0)
    assert up.gateway_id == 0xFF02  # the record wins over the topic
    assert (up.gateway_time, up.transport, up.rssi, up.snr) == (1791460800123, "lora", -87.0, 7.5)
    assert up.decoded == {"temp_c": 23.45, "rh_pct": 40.96, "batt_v": 3.024}


@pytest.mark.parametrize(
    "raw, error",
    [
        (b"{not json", "bad JSON"),
        (b"{}", "payload must be a hex string"),
        (b"[1, 2]", "shorter"),  # only "{" means JSON; anything else is a frame
        (record(v=2), "version"),
        (record(src="0xFF01"), "src"),  # a gateway address
        (record(seq=None), "missing seq"),
        (record(seq=True), "seq is not an integer"),
        (record(payload="zz"), "hex"),
        (record(sec="enc"), "missing fcnt"),
        (record(sec="maybe"), "sec must be"),
        (frame(1, b"", src=0xFF01), "not a node address"),
        (bytes.fromhex("24 00 42 34 12 01 FF 90 00"), "not a DATA frame"),
        (bytes.fromhex("20 80 42 34 12 01 FF 08 01 00 02 00 00 00 68 69 6C DB 84 CD"), "no keys"),
        (b"\x20\x00", "shorter"),
    ],
)
def test_rejected_messages_carry_the_reason(raw, error):
    msg = parse(raw)
    assert msg.uplink is None
    assert error in msg.error
    assert msg.raw == raw


def test_gateway_from_topic():
    assert gateway_from_topic("roclink/in/42/ff01/up") == 0xFF01
    assert gateway_from_topic("site/a/roclink/42/FF02/up") == 0xFF02
    assert gateway_from_topic("roclink/42/1234/up") is None  # a node address
    assert gateway_from_topic("roclink/up") is None


# --- drop reports (docs/GATEWAY_INTERFACE.md §4)

import pathlib
import re

from roclink_hub.record import RecordError, is_rejection_topic, parse_rejection

REJECTED = "roclink/in/42/ff01/rejected"


def annex_example(heading: str) -> bytes:
    """The first JSON example after `heading` in GATEWAY_INTERFACE.md."""
    spec = (pathlib.Path(__file__).parents[1] / "docs" / "GATEWAY_INTERFACE.md").read_text()
    section = spec[spec.index(heading):]
    return re.search(r"```json\n(.*?)```", section, re.S).group(1).encode()


def test_the_protocol_examples_parse():
    up = parse_uplink("roclink/in/42/ff01/up", annex_example("## 3. Uplink record"))
    assert (up.node_id, up.security, up.fcnt, up.rssi) == (0x1234, "enc", 1, -87.0)

    ev = parse_rejection(REJECTED, annex_example("## 4. Drop reports"))
    assert (ev.reason, ev.source, ev.gateway_id, ev.net_id, ev.claimed_src, ev.count, ev.rssi) == (
        "auth", "gateway", 0xFF01, 0x42, 0x1234, 1, -41.0
    )
    # The sample is test vector S6 with its last byte changed.
    s6 = bytes.fromhex(
        "20 82 42 34 12 00 00 09 20 30 00 00 01 00 4E F9 2E C9 36 EA 00 1C DD DA 89 34 89 A6"
        " B0 B2 0E 9F 70 43 7F"
    )
    assert ev.sample[:-1] == s6[:-1] and ev.sample[-1] != s6[-1]


def test_minimal_report_takes_the_gateway_from_the_topic():
    ev = parse_rejection(REJECTED, b'{"kind": "rejected", "reason": "malformed"}')
    assert (ev.reason, ev.gateway_id, ev.net_id, ev.claimed_src, ev.sample) == (
        "malformed", 0xFF01, None, None, None
    )


def test_claimed_src_may_be_any_address():
    ev = parse_rejection(REJECTED, b'{"kind": "rejected", "reason": "addr", "src": "0xFFFF"}')
    assert ev.claimed_src == 0xFFFF


def test_long_frames_are_cut_to_the_sample_size():
    rec = json.dumps({"kind": "rejected", "reason": "auth", "frame": "ab" * 1000}).encode()
    assert len(parse_rejection(REJECTED, rec).sample) == 256


@pytest.mark.parametrize(
    "raw, error",
    [
        (b"\x20\x00", "JSON object"),
        (b"{nope", "bad JSON"),
        (b'{"reason": "auth"}', "kind"),
        (b'{"kind": "rejected"}', "reason"),
        (b'{"kind": "rejected", "reason": "Auth Failed"}', "reason"),
        (b'{"kind": "rejected", "reason": "auth", "count": 0}', "count"),
        (b'{"kind": "rejected", "reason": "auth", "frame": "xyz"}', "hex"),
        (b'{"v": 2, "kind": "rejected", "reason": "auth"}', "version"),
    ],
)
def test_bad_reports(raw, error):
    with pytest.raises(RecordError, match=error):
        parse_rejection(REJECTED, raw)


def test_is_rejection_topic():
    assert is_rejection_topic(REJECTED)
    assert not is_rejection_topic("roclink/in/42/ff01/up")
