import json
import struct

from roclink_hub.config import Config, PublicConfig, Route
from roclink_hub.ratelimit import PublicLimiter
from roclink_hub.record import parse_uplink
from roclink_hub.router import Router

GW = "roclink/in/42/ff01/up"
PUB = "roclink/in/f0/ff01/up"


class Clock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t


def make(routes=(), public_nets=(0xF0,), clock=None):
    cfg = Config(
        db_path="hub.db",
        routes=list(routes),
        public=PublicConfig(nets=list(public_nets), db_path="public.db", max_per_node_per_minute=3),
        schemas={1: "env"},
    )
    return Router(cfg, clock or Clock())


def secured(node=0x0150, schema=20, fcnt=1, net=0x42, payload="29090010d00b"):
    rec = {"net": net, "src": node, "seq": 1, "schema": schema, "payload": payload,
           "sec": "enc", "fcnt": fcnt, "key_gen": 0, "radio": {"rssi": -80}}
    return json.dumps(rec).encode()


def unsecured_frame(net, node=0x0150, schema=1, seq=1):
    return bytes([0x20, 0x00, net]) + struct.pack("<HHBB", node, 0, seq, schema) + b"\x29\x09\x00\x10\xd0\x0b"


SCAV = Route("scavhunt", 0x42, frozenset({20, 21}), ((0x100, 0x1FF),))
EVERYTHING = Route("archive", 0x42)


def test_routes_to_every_matching_service_once():
    r = make([SCAV, EVERYTHING, Route("scavhunt", 0x42)])
    out = r.route(GW, secured(), 1000)
    assert [t for t, _ in out] == ["roclink/svc/scavhunt/up", "roclink/svc/archive/up"]
    body = json.loads(out[0][1])
    assert (body["src"], body["schema"], body["sec"], body["fcnt"], body["hub_time"]) == (
        0x150, 20, "enc", 1, 1000
    )
    assert body["gateway"] == 0xFF01 and body["radio"] == {"rssi": -80.0}


def test_delivered_record_parses_back_to_the_same_uplink():
    out = make([EVERYTHING]).route(GW, secured(schema=1), 1000)
    original = parse_uplink(GW, secured(schema=1))
    again = parse_uplink("roclink/svc/archive/up", out[0][1])
    for field in ("net_id", "node_id", "seq", "schema", "security", "fcnt", "key_gen", "payload",
                  "gateway_id", "rssi"):
        assert getattr(again, field) == getattr(original, field), field
    assert json.loads(out[0][1])["decoded"]["temp_c"] == 23.45


def test_non_matching_and_unparsable_go_nowhere():
    r = make([SCAV])
    assert r.route(GW, secured(schema=99), 1000) == []
    assert r.route(GW, secured(node=0x0300), 1000) == []
    assert r.route(GW, b"{junk", 1000) == []


def test_insecure_uplinks_only_reach_routes_that_allow_them():
    lax = Route("bench", 0x42, allow_insecure=True)
    r = make([EVERYTHING, lax])
    out = r.route(GW, unsecured_frame(0x42), 1000)
    assert [t for t, _ in out] == ["roclink/svc/bench/up"]


def test_public_net_goes_only_to_the_public_topic():
    r = make([EVERYTHING])
    out = r.route(PUB, unsecured_frame(0xF0), 1000)
    assert [t for t, _ in out] == ["roclink/pub/up"]


def test_same_secured_uplink_from_two_gateways_is_delivered_once_ever():
    clock = Clock()
    r = make([EVERYTHING], clock=clock)
    assert len(r.route(GW, secured(), 1000)) == 1
    assert r.route("roclink/in/42/ff02/up", secured(), 1001) == []
    clock.t = 3600  # the anti-replay window isn't a time window
    assert r.route(GW, secured(), 1002) == []


def test_a_replay_to_another_gateway_is_never_delivered():
    r = make([EVERYTHING])
    for fcnt in range(1, 80):
        assert len(r.route(GW, secured(fcnt=fcnt), 1000)) == 1
    # Frame 5, recorded near ff01, replayed to ff02 which never heard it.
    assert r.route("roclink/in/42/ff02/up", secured(fcnt=5), 2000) == []
    assert r.stats["replay"] == 1
    # Out of order, within the window: still new.
    assert len(r.route("roclink/in/42/ff02/up", secured(fcnt=81), 2001)) == 1
    assert len(r.route(GW, secured(fcnt=80), 2002)) == 1


def test_same_unsecured_uplink_is_delivered_once_within_the_time_window():
    clock = Clock()
    r = make([Route("bench", 0x42, allow_insecure=True)], clock=clock)
    assert len(r.route(GW, unsecured_frame(0x42), 1000)) == 1
    assert r.route("roclink/in/42/ff02/up", unsecured_frame(0x42), 1001) == []
    clock.t = 61
    assert len(r.route(GW, unsecured_frame(0x42), 1002)) == 1  # past the window: a service must cope


def test_routes_and_decoders_by_device_type():
    from roclink_hub.registry import DeviceType, NodeEntry
    cfg = Config(
        db_path="hub.db",
        routes=[Route("rooms", 0x42, types=frozenset({"room"}))],
        schemas={20: "event"},
        device_types={"room": DeviceType("room", schemas={20: "text"})},
        nodes=[NodeEntry(0x42, ((0x0150, 0x0150),), "room")],
    )
    r = Router(cfg, Clock())
    out = r.route(GW, secured(node=0x0150, payload="6869"), 1000)
    body = json.loads(out[0][1])
    assert body["device_type"] == "room" and body["decoded"] == {"text": "hi"}
    assert r.route(GW, secured(node=0x0151, fcnt=2), 1000) == []  # not a room: not routed


def test_public_rate_limit_per_node():
    r = make()
    sent = [r.route(PUB, unsecured_frame(0xF0, seq=i), 1000) for i in range(5)]
    assert [len(s) for s in sent] == [1, 1, 1, 0, 0]  # 3 per minute per node


def test_limiter_caps_the_total_across_forged_node_ids():
    clock = Clock()
    lim = PublicLimiter(total_per_min=10, per_node_per_min=5, clock=clock)
    assert sum(lim.allow(0xF0, node) for node in range(1, 100)) == 10
    assert lim.dropped == 89
    clock.t = 6  # a tenth of a minute refills a tenth of the total
    assert sum(lim.allow(0xF0, node) for node in range(100, 200)) == 1
