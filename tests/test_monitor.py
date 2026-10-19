import json

from roclink_hub.monitor import RejectionMonitor, severity
from roclink_hub.record import RejectionEvent

W = 60_000  # window, ms
T = 1_791_460_800_000  # a window boundary


def gw(reason, src=0x1234, net=0x42, count=1, rssi=None, gateway=0xFF01):
    return RejectionEvent(reason, "gateway", gateway, net, src, count=count, rssi=rssi)


def monitor(**kw):
    return RejectionMonitor(window_s=60, burst=kw.get("burst", 5), max_keys=kw.get("max_keys", 100))


def test_severity():
    assert severity("conflict", 1, 20) == "critical"
    assert severity("revoked", 1, 20) == "high"
    assert severity("auth", 19, 20) == "warn"
    assert severity("auth", 20, 20) == "high"  # a burst
    assert severity("net", 10_000, 20) == "info"  # not a burst reason
    assert severity("something_new", 1, 20) == "warn"


def test_counts_per_window_and_flushes_when_it_closes():
    m = monitor()
    m.note_verified(0x42, 0x1234)
    m.record(gw("auth", rssi=-90), T + 1)
    m.record(gw("auth", rssi=-40), T + 2)
    m.record(gw("auth", count=3), T + 3)  # a gateway's own aggregate
    m.record(gw("replay"), T + 4)
    assert m.flush(T + W - 1) == []  # still open
    out = sorted(m.flush(T + W), key=lambda s: s.reason)
    assert [(s.reason, s.count, s.claimed_src) for s in out] == [("auth", 5, 0x1234), ("replay", 1, 0x1234)]
    auth = out[0]
    assert (auth.window_start, auth.first_at, auth.last_at) == (T, T + 1, T + 3)
    assert (auth.rssi_min, auth.rssi_max, auth.severity) == (-90, -40, "high")
    assert m.flush(T + 2 * W) == []


def test_claims_from_unknown_nodes_are_lumped_together():
    m = monitor()
    m.note_verified(0x42, 0x1234)
    for src in range(1, 50):  # a flood of invented addresses
        m.record(gw("not_enrolled", src=src), T)
    m.record(gw("not_enrolled", src=0x1234), T)
    out = {s.claimed_src: s.count for s in m.flush(T, final=True)}
    assert out == {None: 49, 0x1234: 1}


def test_conflict_and_revoked_keep_the_claimed_src():
    m = monitor()
    m.record(gw("conflict", src=0x0777), T)
    m.record(gw("revoked", src=0x0666), T)
    assert {s.claimed_src for s in m.flush(T, final=True)} == {0x0777, 0x0666}


def test_alert_once_when_a_summary_becomes_high():
    m = monitor(burst=3)
    first = m.record(gw("conflict"), T)
    assert first.immediate and first.severity == "critical" and first.count == 1
    assert m.record(gw("conflict"), T + 1) is None  # already alerted this window

    m.note_verified(0x42, 0x1234)
    assert m.record(gw("auth"), T) is None
    assert m.record(gw("auth"), T) is None
    burst = m.record(gw("auth"), T)
    assert burst.severity == "high" and burst.count == 3
    assert m.record(gw("auth"), T) is None

    # Next window: alert again.
    assert m.record(gw("conflict"), T + W).immediate
    summaries = {s.reason: s for s in m.flush(T + W)}
    assert summaries["conflict"].count == 2 and not summaries["conflict"].immediate


def test_max_keys_falls_back_to_counting_by_reason():
    m = monitor(max_keys=2)
    m.record(gw("auth", gateway=0xFF01), T)
    m.record(gw("auth", gateway=0xFF02), T)
    m.record(gw("auth", gateway=0xFF03), T)  # full: by reason only
    m.record(gw("weird_reason", gateway=0xFF04), T)  # unknown reasons share one key
    m.record(gw("other_weird", gateway=0xFF05), T)
    out = sorted((s.gateway_id or 0, s.reason, s.count) for s in m.flush(T, final=True))
    assert out == [(0, "auth", 1), (0, "other", 2), (0xFF01, "auth", 1), (0xFF02, "auth", 1)]


def test_summary_json():
    m = monitor()
    ev = RejectionEvent("malformed_record", "hub", 0xFF01, sample=b"{x", detail="bad JSON")
    alert = m.record(ev, T)
    rec = json.loads(alert.to_json())
    assert rec == {
        "v": 1, "kind": "alert", "severity": "high", "reason": "malformed_record",
        "source": "hub", "gateway": 0xFF01, "count": 1, "window_start": T, "window_s": 60,
        "first_at": T, "last_at": T, "sample": "7b78", "detail": "bad JSON",
    }
