import pytest

from scavhunt import messages as m


def test_box_message_round_trip():
    for token in (b"\x01", b"\x04\xa1\xb2\xc3\xd4\xe5\xf6", b"x" * 16):
        assert m.parse_box_message(m.SCHEMA_WIN, m.box_message(token)) == (token, None)
    assert m.parse_box_message(m.SCHEMA_HINT, m.box_message(b"AB", 3)) == (b"AB", 3)


@pytest.mark.parametrize(
    "schema, payload",
    [(1, b""), (1, b"\x01"), (1, b"\x02\x01A"), (1, b"\x01\x00"), (1, b"\x01\x11" + b"x" * 17),
     (1, b"\x01\x02A"), (1, b"\x01\x01AB"), (4, b"\x01\x01A"), (4, b"\x01\x01A\x01\x02")],
)
def test_bad_box_messages(schema, payload):
    with pytest.raises(m.MessageError):
        m.parse_box_message(schema, payload)


def test_verdict_layout():
    assert m.verdict(3, 2, 7, 0x0201) == bytes([1, 3, 2, 7, 0x01, 0x02])
    assert m.verdict(0, 300, 300) == bytes([1, 0, 255, 255, 0, 0])


def test_control():
    assert m.control(m.CONTROL_RESET) == b"\x01\x01"


def test_operator_tokens():
    assert m.parse_token("04:A1:B2:C3") == bytes.fromhex("04a1b2c3")
    assert m.parse_token("text:RED7") == b"RED7"
    with pytest.raises(m.MessageError, match="text:"):
        m.parse_token("RED7")
    with pytest.raises(m.MessageError):
        m.parse_token("text:" + "x" * 17)
