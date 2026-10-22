import pytest

from roclink_hub import frame

# PROTOCOL.md §11
V1 = bytes.fromhex("20 03 42 34 12 00 00 07 01 68 69")
V2 = bytes.fromhex("24 04 42 01 FF 34 12 07 00")
V3 = bytes.fromhex("20 09 42 01 FF 34 12 90 0A 3C 00")
V5 = bytes.fromhex("20 30 42 34 12 00 00 08 03 EF BE AA")
S2 = bytes.fromhex("20 80 42 34 12 01 FF 08 01 00 02 00 00 00 68 69 6C DB 84 CD")
S1 = bytes.fromhex(
    "20 83 42 34 12 00 00 07 20 30 01 00 00 00 D8 61 5E 8A A3 76 B1 7E"
    " 23 01 D5 F2 D7 E3 52 0A 64 69 E1 7F 64"
)


def test_uplink_data():
    f = frame.decode(V1)
    assert (f.type, f.flags, f.net_id, f.src, f.dst, f.seq, f.schema) == (0, 3, 0x42, 0x1234, 0, 7, 1)
    assert f.payload == b"hi"
    assert not f.secured


def test_ack():
    f = frame.decode(V2)
    assert (f.type, f.flags, f.src, f.dst, f.seq, f.schema, f.payload) == (1, 4, 0xFF01, 0x1234, 7, 0, b"")


def test_downlink():
    f = frame.decode(V3)
    assert (f.src, f.dst, f.seq, f.schema, f.payload) == (0xFF01, 0x1234, 0x90, 10, b"\x3c\x00")


def test_relayed_is_dropped():
    with pytest.raises(frame.FrameError, match="relayed"):
        frame.decode(V5)


def test_secured_auth_4_byte_tag():
    f = frame.decode(S2)
    assert f.secured and f.sec_ctrl == 0x00 and f.fcnt == 2
    assert f.payload == b"hi"
    assert f.tag == bytes.fromhex("6C DB 84 CD")


def test_secured_enc_8_byte_tag():
    f = frame.decode(S1)
    assert f.sec_ctrl == 0x30 and f.fcnt == 1
    assert len(f.payload) == 13 and len(f.tag) == 8


@pytest.mark.parametrize(
    "raw, reason",
    [
        (V1[:8], "shorter"),
        (bytes([0x40]) + V1[1:], "version"),  # ver 2
        (bytes([0x28]) + V1[1:], "reserved frame type"),  # type 2
        (V2 + b"x", "ACK"),
        (bytes.fromhex("20 80 42 34 12 01 FF 08 01 00 02 00"), "security header"),
        (bytes.fromhex("20 80 42 34 12 01 FF 08 01 10 02 00 00 00 01 02 03"), "tag"),
    ],
)
def test_rejects(raw, reason):
    with pytest.raises(frame.FrameError, match=reason):
        frame.decode(raw)
