"""A writer process: the only process that writes to its database.

There is one writer per database file: one for hub.db (private and service
uplinks, kept for good) and, with a public net, one for public.db (pruned to
its retention period). Each has its own queue, so a flood on the public net
can't slow down private storage.

A writer takes Messages off its queue in batches and writes each batch in one
transaction. Batching keeps commits (and SD card writes) few, and readers see
a batch all at once or not at all.

Because nothing else writes to its file, check-then-insert needs no locking:
the duplicate check below can't race another writer.
"""

import json
import logging
import queue as queue_module
import sqlite3
import time
from collections import Counter

from . import db
from .process import STOP, child_init, now_ms
from .monitor import RejectionSummary
from .record import Message, Uplink

log = logging.getLogger(__name__)


def run(cfg, db_path: str, retention_ms: int | None, queue, ready) -> None:
    """Write `queue` into `db_path`. With retention_ms, also delete anything
    older than that, every prune_interval_s."""
    parent_gone = child_init(cfg.log_level)
    conn = db.connect_writer(db_path)
    version = db.migrate(conn)
    kept = f"kept {retention_ms / 3_600_000:g} h" if retention_ms else "kept for good"
    log.info("database %s ready (schema %d, %s)", db_path, version, kept)
    ready.set()

    w = cfg.writer
    stats: Counter = Counter()
    next_report = time.monotonic() + w.stats_interval_s
    next_prune = time.monotonic()  # prune once at start: the hub may have been down
    while True:
        batch, finished = next_batch(queue, w.batch_size, w.flush_interval_s)
        if batch:
            write_batch(conn, batch, w.dedupe_window_s * 1000, stats)
        if retention_ms and time.monotonic() >= next_prune:
            stats["pruned"] += db.prune(conn, now_ms() - retention_ms)
            next_prune = time.monotonic() + w.prune_interval_s
        if time.monotonic() >= next_report:
            log.info("last %ds: %s", w.stats_interval_s, _summary(stats))
            stats.clear()
            next_report = time.monotonic() + w.stats_interval_s
        if finished or (not batch and parent_gone()):
            break

    db.close_writer(conn)
    log.info("stopped")


def next_batch(queue, max_items: int, max_wait_s: float) -> tuple[list[Message], bool]:
    """Wait for a message, then take whatever else arrives within max_wait_s.

    Returns the batch and whether STOP was seen. Waits at most a second for
    the first message, so the caller can check on the supervisor now and then.
    """
    batch: list[Message] = []
    try:
        item = queue.get(timeout=1.0)
    except queue_module.Empty:
        return batch, False
    deadline = time.monotonic() + max_wait_s
    while True:
        if item is STOP:
            return batch, True
        batch.append(item)
        remaining = deadline - time.monotonic()
        if len(batch) >= max_items or remaining <= 0:
            return batch, False
        try:
            item = queue.get(timeout=remaining)
        except queue_module.Empty:
            return batch, False


def write_batch(conn: sqlite3.Connection, batch: list, window_ms: int, stats: Counter):
    """Write a batch of Messages and RejectionSummaries in one transaction.
    A message that fails is recorded as rejected; it doesn't cost the rest of
    the batch."""
    conn.execute("BEGIN IMMEDIATE")
    for item in batch:
        conn.execute("SAVEPOINT item")
        try:
            if isinstance(item, RejectionSummary):
                insert_rejection(conn, item)
                status = "rejection summaries"
            else:
                status = store(conn, item, window_ms)
        except Exception as e:  # a bug or bad data, not a reason to drop the batch
            conn.execute("ROLLBACK TO item")
            if isinstance(item, RejectionSummary):
                log.exception("could not store a rejection summary")
                status = "failed"
            else:
                log.exception("could not store message on %s", item.topic)
                _insert_message(conn, item, "rejected", f"store failed: {e}")
                status = "rejected"
        conn.execute("RELEASE item")
        stats[status] += 1
    conn.execute("COMMIT")


def store(conn: sqlite3.Connection, msg: Message, window_ms: int) -> str:
    """Write one message. Returns its status: stored, duplicate or rejected."""
    up = msg.uplink
    if up is None:
        log.warning("rejected message on %s: %s", msg.topic, msg.error)
        _insert_message(conn, msg, "rejected", msg.error)
        return "rejected"

    uplink_id = find_duplicate(conn, up, msg.received_at, window_ms)
    status = "stored" if uplink_id is None else "duplicate"
    message_id = _insert_message(conn, msg, status, None)
    if uplink_id is None:
        uplink_id = _insert_uplink(conn, up, msg.received_at)
        _insert_readings(conn, uplink_id, up, msg.received_at)
        _upsert_node(conn, up, msg.received_at)
    _insert_reception(conn, uplink_id, message_id, up, msg.received_at)
    return status


def find_duplicate(conn: sqlite3.Connection, up: Uplink, now: int, window_ms: int) -> int | None:
    """The id of an uplink this one repeats, if any (PROTOCOL.md §9.2, §9.5).

    Several gateways may forward the same uplink, and a gateway restart can
    forward one again.
    - Secured: the same frame counter is the same frame, whenever it arrives.
    - Unsecured: the same seq, schema and payload within the dedupe window.
      The 8-bit seq wraps, so an old match isn't a duplicate.
    """
    if up.fcnt is not None:
        row = conn.execute(
            "SELECT id FROM uplinks"
            " WHERE net_id = ? AND node_id = ? AND key_gen = ? AND fcnt = ?",
            (up.net_id, up.node_id, up.key_gen, up.fcnt),
        ).fetchone()
    else:
        row = conn.execute(
            "SELECT id FROM uplinks"
            " WHERE net_id = ? AND node_id = ? AND received_at >= ?"
            " AND fcnt IS NULL AND seq = ? AND schema = ? AND payload = ?"
            " ORDER BY id DESC LIMIT 1",
            (up.net_id, up.node_id, now - window_ms, up.seq, up.schema, up.payload),
        ).fetchone()
    return row[0] if row else None


def insert_rejection(conn: sqlite3.Connection, s: RejectionSummary) -> None:
    conn.execute(
        "INSERT INTO rejections (window_start, window_s, source, gateway_id, net_id, claimed_src,"
        " reason, severity, count, first_at, last_at, rssi_min, rssi_max, sample, detail)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        (
            s.window_start,
            s.window_s,
            s.source,
            s.gateway_id,
            s.net_id,
            s.claimed_src,
            s.reason,
            s.severity,
            s.count,
            s.first_at,
            s.last_at,
            s.rssi_min,
            s.rssi_max,
            s.sample,
            s.detail,
        ),
    )


def _insert_message(conn, msg: Message, status: str, error: str | None) -> int:
    cur = conn.execute(
        "INSERT INTO messages (received_at, topic, raw, status, error) VALUES (?, ?, ?, ?, ?)",
        (msg.received_at, msg.topic, msg.raw, status, error),
    )
    return cur.lastrowid


def _insert_uplink(conn, up: Uplink, received_at: int) -> int:
    cur = conn.execute(
        "INSERT INTO uplinks (received_at, net_id, node_id, dst, seq, schema, flags, security,"
        " fcnt, key_gen, payload, decoder, decoded, decode_error)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        (
            received_at,
            up.net_id,
            up.node_id,
            up.dst,
            up.seq,
            up.schema,
            up.flags,
            up.security,
            up.fcnt,
            up.key_gen,
            up.payload,
            up.decoder,
            json.dumps(up.decoded) if up.decoded is not None else None,
            up.decode_error,
        ),
    )
    return cur.lastrowid


def _insert_readings(conn, uplink_id: int, up: Uplink, received_at: int) -> None:
    if not up.decoded:
        return
    conn.executemany(
        "INSERT INTO readings (uplink_id, net_id, node_id, received_at, metric, value)"
        " VALUES (?, ?, ?, ?, ?, ?)",
        [
            (uplink_id, up.net_id, up.node_id, received_at, metric, value)
            for metric, value in up.decoded.items()
            if isinstance(value, (int, float)) and not isinstance(value, bool)
        ],
    )


def _upsert_node(conn, up: Uplink, received_at: int) -> None:
    conn.execute(
        "INSERT INTO nodes (net_id, node_id, first_seen, last_seen, last_schema, uplink_count)"
        " VALUES (?, ?, ?, ?, ?, 1)"
        " ON CONFLICT (net_id, node_id) DO UPDATE SET"
        "   last_seen = excluded.last_seen,"
        "   last_schema = excluded.last_schema,"
        "   uplink_count = uplink_count + 1",
        (up.net_id, up.node_id, received_at, received_at, up.schema),
    )


def _insert_reception(conn, uplink_id: int, message_id: int, up: Uplink, received_at: int):
    conn.execute(
        "INSERT INTO receptions (uplink_id, message_id, received_at, gateway_id, gateway_time,"
        " transport, rssi, snr) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        (
            uplink_id,
            message_id,
            received_at,
            up.gateway_id,
            up.gateway_time,
            up.transport,
            up.rssi,
            up.snr,
        ),
    )


def _summary(stats: Counter) -> str:
    stats = +stats  # drop zero counts
    if not stats:
        return "no messages"
    return ", ".join(f"{n} {status}" for status, n in sorted(stats.items()))
