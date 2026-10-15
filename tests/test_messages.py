import pytest

from scavhunt import messages as m


def test_token_message_round_trip():
    for token in (b"\x01", b"\x04\xa1\xb2\xc3\xd4\xe5\xf6", b"x" * 16):
        assert m.parse_token_message(m.token_message(token)) == token


@pytest.mark.parametrize(
    "payload",
    [b"", b"\x01", b"\x02\x01A", b"\x01\x00", b"\x01\x11" + b"x" * 17, b"\x01\x02A", b"\x01\x01AB"],
)
def test_bad_token_messages(payload):
    with pytest.raises(m.MessageError):
        m.parse_token_message(payload)


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
