import json

import pytest

from roclink_hub.config import Config, DownlinkConfig, PublicConfig, Route
from roclink_hub.downlink import Downlinks
from roclink_hub.registry import ALWAYS_LISTENING, DeviceType, NodeEntry

import sqlite3

SVC = "roclink/svc/valves/down"


class Clock:
    def __init__(self):
        self.t = 1_000_000

    def __call__(self):
        return self.t


def make(**dl):
    cfg = Config(
        db_path="hub.db",
        routes=[Route("valves", 0x42, frozenset({10}))],
        public=PublicConfig(nets=[0xF0], db_path="public.db"),
        device_types={"valve": DeviceType("valve", ALWAYS_LISTENING, {})},
        nodes=[NodeEntry(0x42, ((0x0200, 0x02FF),), "valve")],
        downlink=DownlinkConfig(**dl),
    )
    sent = []
    clock = Clock()
    q = Downlinks(cfg, sqlite3.connect(":memory:", isolation_level=None),
                  lambda t, b: sent.append((t, json.loads(b))), clock)
    return q, sent, clock


def request(q, node="0x1234", schema=10, **extra):
    q.request(SVC, json.dumps({"net": 0x42, "node": node, "schema": schema, "payload": "3c00",
                               "ref": "r1", **extra}).encode())


def uplink(q, node, gw="ff01", fcnt=7):
    q.uplink(f"roclink/in/42/{gw}/up", json.dumps(
        {"net": 0x42, "src": node, "seq": 1, "schema": 1, "payload": "", "sec": "enc",
         "fcnt": fcnt, "key_gen": 0}).encode())


def test_waits_for_the_node_then_offers_it_for_its_listen_window():
    q, sent, _ = make()
    request(q)
    (topic, body), = sent
    assert topic == "roclink/svc/valves/down_status"
    assert body["status"] == "queued" and body["ref"] == "r1"
    seq = body["seq"]
    uplink(q, 0x1234, gw="ff02")
    topic, offer = sent[-1]
    assert topic == "roclink/out/42/ff02/down"  # the gateway that heard it
    assert (offer["node"], offer["schema"], offer["seq"], offer["payload"]) == ("0x1234", 10, seq, "3c00")
    assert offer["send"] == "window" and offer["binding"] is None and offer["ack"] is True
    q.status("roclink/in/42/ff02/down_status", json.dumps({"id": offer["id"], "status": "acked"}).encode())
    assert sent[-1][1] == {"ref": "r1", "id": offer["id"], "status": "acked"}


def test_always_listening_is_sent_now_bound_to_the_latest_uplink():
    q, sent, _ = make()
    uplink(q, 0x0250, fcnt=9)
    uplink(q, 0x0250, fcnt=8, gw="ff02")  # late copy of an older frame: binding stays 9
    request(q, node="0x0250")
    topic, offer = sent[-1]
    assert topic == "roclink/out/42/ff02/down"
    assert offer["send"] == "now" and offer["binding"] == 9


def test_reoffers_keep_the_seq_and_move_to_the_newest_gateway():
    q, sent, clock = make()
    uplink(q, 0x1234)
    request(q)
    first = sent[-1][1]
    uplink(q, 0x1234, gw="ff03")
    q.status("roclink/in/42/ff01/down_status", json.dumps({"id": first["id"], "status": "expired"}).encode())
    topic, second = sent[-1]
    assert topic == "roclink/out/42/ff03/down"
    assert second["seq"] == first["seq"] and second["id"] == first["id"]
    # A stale status from the old gateway changes nothing.
    q.status("roclink/in/42/ff01/down_status", json.dumps({"id": first["id"], "status": "acked"}).encode())
    assert sent[-1][0] == "roclink/out/42/ff03/down"


def test_gives_up_after_max_attempts_and_tells_the_service():
    q, sent, clock = make(max_attempts=2)
    uplink(q, 0x1234)
    request(q)
    dl = sent[-1][1]["id"]
    for _ in range(2):
        clock.t += 700_000  # past the offer deadline, with no status from the gateway
        q.tick()
    assert sent[-1][1]["status"] == "failed" and sent[-1][1]["id"] == dl


def test_consecutive_downlinks_get_consecutive_seqs():
    q, sent, _ = make()
    request(q)
    request(q)
    a, b = [m["seq"] for t, m in sent if m.get("status") == "queued"]
    assert b == (a + 1) % 256


@pytest.mark.parametrize("extra, why", [
    ({"schema": 11}, "no route"),
    ({"node": "0xFF01"}, "outside"),
    ({"net": 0xF0}, "public"),
    ({"ttl_s": 0}, "ttl_s"),
    ({"payload": "zz"}, ""),
])
def test_refuses_what_the_service_may_not_send(extra, why):
    q, sent, _ = make()
    rec = {"net": 0x42, "node": "0x1234", "schema": 10, "payload": "3c00", "ref": 5, **extra}
    q.request(SVC, json.dumps(rec).encode())
    (topic, body), = sent
    assert body["status"] == "refused" and why in body["detail"] and body["ref"] == 5
