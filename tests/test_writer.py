import json
import sqlite3
from collections import Counter

import pytest

from roclink_hub import db
from roclink_hub.record import parse_message
from roclink_hub.writer import store, write_batch

PORTS = {1: "env"}
WINDOW_MS = 60_000
ENV = "29090010d00b"  # 23.45 C, 40.96 %RH, 3.024 V


def msg(at=1_000, topic="roclink/in/42/ff01/up", **fields):
    rec = {"net": 0x42, "src": "0x1234", "seq": 7, "schema": 1, "payload": ENV}
    rec.update(fields)
    return parse_message(topic, json.dumps(rec).encode(), at, PORTS)


@pytest.fixture
def conn(tmp_path):
    c = db.connect_writer(tmp_path / "hub.db")
    db.migrate(c)
    yield c
    c.close()


def count(conn, table):
    return conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]


def test_stores_uplink_readings_node_and_reception(conn):
    assert store(conn, msg(radio={"rssi": -80}), WINDOW_MS) == "stored"
    up = conn.execute("SELECT * FROM uplinks").fetchone()
    assert json.loads(up[13]) == {"temp_c": 23.45, "rh_pct": 40.96, "batt_v": 3.024}
    readings = dict(conn.execute("SELECT metric, value FROM readings").fetchall())
    assert readings == {"temp_c": 23.45, "rh_pct": 40.96, "batt_v": 3.024}
    assert conn.execute("SELECT node_id, uplink_count FROM nodes").fetchall() == [(0x1234, 1)]
    assert conn.execute("SELECT gateway_id, rssi FROM receptions").fetchall() == [(0xFF01, -80.0)]


def test_unsecured_duplicate_from_a_second_gateway(conn):
    assert store(conn, msg(at=1_000), WINDOW_MS) == "stored"
    assert store(conn, msg(at=1_200, topic="roclink/in/42/ff02/up"), WINDOW_MS) == "duplicate"
    assert count(conn, "uplinks") == 1
    assert count(conn, "messages") == 2
    gateways = conn.execute("SELECT gateway_id FROM receptions ORDER BY id").fetchall()
    assert gateways == [(0xFF01,), (0xFF02,)]
    assert conn.execute("SELECT uplink_count FROM nodes").fetchone()[0] == 1


def test_unsecured_same_seq_is_new_after_the_window_or_with_new_payload(conn):
    store(conn, msg(at=1_000), WINDOW_MS)
    assert store(conn, msg(at=1_000 + WINDOW_MS + 1), WINDOW_MS) == "stored"  # seq wrapped
    assert store(conn, msg(at=1_000 + WINDOW_MS + 2, payload="000000000000"), WINDOW_MS) == "stored"
    assert count(conn, "uplinks") == 3


def test_secured_duplicates_go_by_frame_counter(conn):
    secured = dict(sec="enc", key_gen=0)
    assert store(conn, msg(at=1_000, fcnt=5, **secured), WINDOW_MS) == "stored"
    # Hours later, still the same frame.
    assert store(conn, msg(at=10_000_000, fcnt=5, **secured), WINDOW_MS) == "duplicate"
    # Same seq, next counter: a new frame.
    assert store(conn, msg(at=1_001, fcnt=6, **secured), WINDOW_MS) == "stored"
    # Same counter under a new key generation: a new frame.
    assert store(conn, msg(at=1_002, fcnt=5, sec="enc", key_gen=1), WINDOW_MS) == "stored"
    assert count(conn, "uplinks") == 3


def test_rejected_message_is_kept_with_its_reason(conn):
    bad = parse_message("roclink/in/42/ff01/up", b"{oops", 1_000, PORTS)
    assert store(conn, bad, WINDOW_MS) == "rejected"
    assert conn.execute("SELECT raw, status FROM messages").fetchone() == (b"{oops", "rejected")
    assert "bad JSON" in conn.execute("SELECT error FROM messages").fetchone()[0]
    assert count(conn, "uplinks") == 0


def test_a_failing_message_doesnt_lose_the_batch(conn):
    good, broken, also_good = msg(seq=1), msg(seq=2), msg(seq=3)
    broken.uplink.decoded = {"x": object()}  # not JSON-serialisable
    stats = Counter()
    write_batch(conn, [good, broken, also_good], WINDOW_MS, stats)
    assert stats == {"stored": 2, "rejected": 1}
    assert [r[0] for r in conn.execute("SELECT seq FROM uplinks ORDER BY id")] == [1, 3]
    assert "store failed" in conn.execute(
        "SELECT error FROM messages WHERE status = 'rejected'"
    ).fetchone()[0]
    assert not conn.in_transaction


def test_migrate_is_idempotent_and_refuses_a_newer_schema(conn):
    assert db.migrate(conn) == db.SCHEMA_VERSION
    conn.execute(f"PRAGMA user_version = {db.SCHEMA_VERSION + 1}")
    with pytest.raises(RuntimeError, match="newer"):
        db.migrate(conn)


def test_readers_cannot_write(conn, tmp_path):
    reader = db.connect_reader(tmp_path / "hub.db")
    with pytest.raises(sqlite3.OperationalError):
        reader.execute("DELETE FROM uplinks")
    reader.close()


def test_prune_deletes_only_what_is_older_than_the_cutoff(conn):
    store(conn, msg(at=1_000, seq=1, src="0x0001"), WINDOW_MS)  # old node, old uplink
    store(conn, msg(at=5_000, seq=2), WINDOW_MS)  # old uplink...
    store(conn, msg(at=5_500, seq=2, topic="roclink/in/42/ff02/up"), WINDOW_MS)  # ...heard again
    store(conn, msg(at=9_000, seq=3), WINDOW_MS)  # new
    store(conn, parse_message("t", b"{bad", 2_000, PORTS), WINDOW_MS)  # old rejected message

    # Cutoff between the uplink (5000) and its second reception (5500):
    # deleting the uplink must take the newer reception with it.
    assert db.prune(conn, cutoff=5_200) == 2 + 3  # uplinks 1, 2; messages 1, 2, rejected
    assert [r[0] for r in conn.execute("SELECT seq FROM uplinks")] == [3]
    assert count(conn, "receptions") == 1
    assert count(conn, "readings") == 3
    assert conn.execute("SELECT received_at FROM messages ORDER BY id").fetchall() == [(5_500,), (9_000,)]
    assert [r[0] for r in conn.execute("SELECT node_id FROM nodes")] == [0x1234]
    assert conn.execute("PRAGMA foreign_key_check").fetchall() == []
    assert not conn.in_transaction


def test_prune_works_in_chunks_and_stops_at_max_chunks(conn, monkeypatch):
    monkeypatch.setattr(db, "PRUNE_CHUNK", 2)
    for i in range(5):
        store(conn, msg(at=1_000 + i, seq=i), WINDOW_MS)
    assert db.prune(conn, cutoff=10_000, max_chunks=2) == 4  # two chunks of two uplinks
    assert count(conn, "uplinks") == 1
    assert db.prune(conn, cutoff=10_000) == 1 + 5  # the last uplink, then the messages
    assert count(conn, "messages") == 0 and count(conn, "nodes") == 0


def summary(**kw):
    from roclink_hub.monitor import RejectionSummary

    fields = dict(window_start=60_000, window_s=60, source="gateway", gateway_id=0xFF01,
                  net_id=0x42, claimed_src=0x1234, reason="auth", severity="high", count=40,
                  first_at=60_001, last_at=119_000, rssi_min=-45.0, rssi_max=-40.0,
                  sample=b"\x20\x82", detail=None)
    fields.update(kw)
    return RejectionSummary(**fields)


def test_rejection_summaries_are_stored_alongside_messages(conn):
    stats = Counter()
    write_batch(conn, [msg(), summary(), msg(seq=8)], WINDOW_MS, stats)
    assert stats == {"stored": 2, "rejection summaries": 1}
    row = conn.execute(
        "SELECT reason, severity, count, claimed_src, rssi_min, sample FROM rejections"
    ).fetchone()
    assert row == ("auth", "high", 40, 0x1234, -45.0, b"\x20\x82")


def test_a_bad_summary_doesnt_lose_the_batch(conn):
    stats = Counter()
    write_batch(conn, [summary(reason=None), msg()], WINDOW_MS, stats)  # reason is NOT NULL
    assert stats == {"failed": 1, "stored": 1}
    assert count(conn, "rejections") == 0


def test_prune_also_drops_old_rejection_summaries(conn):
    write_batch(conn, [summary(window_start=0), summary(window_start=120_000)], WINDOW_MS, Counter())
    db.prune(conn, cutoff=60_000)
    assert [r[0] for r in conn.execute("SELECT window_start FROM rejections")] == [120_000]


def test_a_schema_1_database_is_upgraded_in_place(tmp_path):
    old = db.connect_writer(tmp_path / "old.db")
    old.executescript(f"BEGIN; {db.MIGRATIONS[0]}; PRAGMA user_version = 1; COMMIT;")
    # An uplink as a version-1 hub wrote it, when the column was still "port".
    old.execute(
        "INSERT INTO uplinks (received_at, net_id, node_id, dst, seq, port, flags, security, payload)"
        " VALUES (1, 66, 4660, 0, 7, 1, 0, 'none', x'')"
    )
    assert db.migrate(old) == db.SCHEMA_VERSION
    assert old.execute("SELECT schema FROM uplinks").fetchall() == [(1,)]  # kept, renamed
    assert store(old, msg(at=2_000_000), WINDOW_MS) == "stored"
    write_batch(old, [summary()], WINDOW_MS, Counter())
    assert count(old, "rejections") == 1
    old.close()
