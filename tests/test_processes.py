"""The writer in its own process, with readers in this one, as in production."""

import json
import multiprocessing
import time

from roclink_hub import api, db, writer
from roclink_hub.config import Config
from roclink_hub.process import STOP
from roclink_hub.record import parse_message

N = 2_000


def message(i: int):
    rec = {"net": 0x42, "src": 1 + i % 10, "seq": i % 256, "schema": 1, "payload": "29090010d00b"}
    return parse_message("roclink/in/42/ff01/up", json.dumps(rec).encode(), 1_000 + i * 100, {1: "env"})


def test_readers_read_while_the_writer_writes(tmp_path):
    cfg = Config(db_path=str(tmp_path / "hub.db"), schemas={1: "env"}, log_level="WARNING")
    ctx = multiprocessing.get_context("spawn")
    queue, ready = ctx.Queue(maxsize=100), ctx.Event()
    w = ctx.Process(target=writer.run, args=(cfg, cfg.db_path, None, queue, ready), name="writer")
    w.start()
    assert ready.wait(10)

    reader = db.connect_reader(cfg.db_path)
    seen = []
    for i in range(N):
        queue.put(message(i))  # maxsize=100 means the writer is draining as we go
        if i % 100 == 0:
            # Every read sees a whole number of committed batches, never an error.
            uplinks = reader.execute("SELECT COUNT(*) FROM uplinks").fetchone()[0]
            readings = reader.execute("SELECT COUNT(*) FROM readings").fetchone()[0]
            assert readings == 3 * uplinks
            seen.append(uplinks)
    queue.put(STOP)
    w.join(30)

    assert w.exitcode == 0
    assert seen == sorted(seen) and seen[-1] > 0  # progress was visible while writing
    assert reader.execute("SELECT COUNT(*) FROM uplinks").fetchone()[0] == N
    assert reader.execute("SELECT SUM(uplink_count) FROM nodes").fetchone()[0] == N

    # The API's queries, over the same read-only connection type.
    assert len(api.nodes(reader, {})) == 10
    latest = api.uplinks(reader, {"node": "0x1", "limit": "2"})
    assert [u["node_id"] for u in latest] == [1, 1]
    assert latest[0]["decoded"]["temp_c"] == 23.45
    series = api.readings(reader, {"node": "1", "metric": "temp_c", "limit": "5"})
    assert len(series) == 5 and series == sorted(series)
    assert api.health(reader, {})["schema"] == db.SCHEMA_VERSION
    reader.close()


def test_writer_commits_everything_queued_before_stop(tmp_path):
    cfg = Config(db_path=str(tmp_path / "hub.db"), log_level="WARNING")
    ctx = multiprocessing.get_context("spawn")
    queue, ready = ctx.Queue(), ctx.Event()
    for i in range(500):
        queue.put(message(i))
    queue.put(STOP)
    start = time.monotonic()
    w = ctx.Process(target=writer.run, args=(cfg, cfg.db_path, None, queue, ready), name="writer")
    w.start()
    w.join(30)
    assert w.exitcode == 0
    assert time.monotonic() - start < 30
    reader = db.connect_reader(cfg.db_path)
    assert reader.execute("SELECT COUNT(*) FROM messages").fetchone()[0] == 500
    reader.close()
