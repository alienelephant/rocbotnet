"""SQLite schema and connections.

One writer, many readers:
- Only the writer process calls connect_writer(). It creates the schema and
  is the only connection that ever writes.
- Every other process (the API, a web UI, the sqlite3 shell) uses
  connect_reader(), which opens the file read-only.
- WAL mode lets readers keep reading while the writer commits. A reader sees
  the database as of the start of its read transaction, and never blocks the
  writer.

Times are unix milliseconds (UTC). Addresses are stored as integers.
"""

import sqlite3
from pathlib import Path

# Each entry moves the schema up one version (PRAGMA user_version).
# Append new migrations; never edit one that has shipped.
MIGRATIONS = [
    """
    -- Every MQTT message, verbatim, whatever happened to it.
    CREATE TABLE messages (
        id          INTEGER PRIMARY KEY,
        received_at INTEGER NOT NULL,  -- hub clock
        topic       TEXT    NOT NULL,
        raw         BLOB    NOT NULL,
        status      TEXT    NOT NULL,  -- 'stored', 'duplicate' or 'rejected'
        error       TEXT               -- why it was rejected
    );
    CREATE INDEX messages_received_at ON messages(received_at);

    -- Each node the hub has heard from.
    CREATE TABLE nodes (
        net_id       INTEGER NOT NULL,
        node_id      INTEGER NOT NULL,
        first_seen   INTEGER NOT NULL,
        last_seen    INTEGER NOT NULL,
        last_port    INTEGER NOT NULL,
        uplink_count INTEGER NOT NULL,
        PRIMARY KEY (net_id, node_id)
    );

    -- One row per distinct uplink, however many gateways forwarded it.
    CREATE TABLE uplinks (
        id           INTEGER PRIMARY KEY,
        received_at  INTEGER NOT NULL,  -- first reception, hub clock
        net_id       INTEGER NOT NULL,
        node_id      INTEGER NOT NULL,
        dst          INTEGER NOT NULL,
        seq          INTEGER NOT NULL,
        port         INTEGER NOT NULL,
        flags        INTEGER NOT NULL,
        security     TEXT    NOT NULL,  -- 'none', 'auth' or 'enc'
        fcnt         INTEGER,           -- secured only
        key_gen      INTEGER,           -- secured only
        payload      BLOB    NOT NULL,
        decoder      TEXT,              -- the payload's decoder, if it has one
        decoded      TEXT,              -- JSON object from the decoder
        decode_error TEXT
    );
    CREATE INDEX uplinks_node_time ON uplinks(net_id, node_id, received_at);
    -- Secured duplicates are exact: the same frame counter (PROTOCOL.md §9.5).
    CREATE UNIQUE INDEX uplinks_secured_unique
        ON uplinks(net_id, node_id, key_gen, fcnt) WHERE fcnt IS NOT NULL;

    -- Every time a gateway forwarded an uplink. Several gateways may hear one.
    CREATE TABLE receptions (
        id           INTEGER PRIMARY KEY,
        uplink_id    INTEGER NOT NULL REFERENCES uplinks(id),
        message_id   INTEGER NOT NULL REFERENCES messages(id),
        received_at  INTEGER NOT NULL,  -- hub clock
        gateway_id   INTEGER,
        gateway_time INTEGER,           -- gateway clock, if it sent one
        transport    TEXT,
        rssi         REAL,
        snr          REAL
    );
    CREATE INDEX receptions_uplink ON receptions(uplink_id);

    -- Numeric values from decoded payloads, for charts.
    CREATE TABLE readings (
        uplink_id   INTEGER NOT NULL REFERENCES uplinks(id),
        net_id      INTEGER NOT NULL,
        node_id     INTEGER NOT NULL,
        received_at INTEGER NOT NULL,
        metric      TEXT    NOT NULL,
        value       REAL    NOT NULL,
        PRIMARY KEY (uplink_id, metric)
    ) WITHOUT ROWID;
    CREATE INDEX readings_series ON readings(net_id, node_id, metric, received_at);
    """,
    """
    -- Dropped frames, counted per window (monitor.py, GATEWAY_INTERFACE.md §4).
    -- Every field but reason came from a dropped frame: claimed_src is a claim.
    CREATE TABLE rejections (
        id           INTEGER PRIMARY KEY,
        window_start INTEGER NOT NULL,
        window_s     INTEGER NOT NULL,
        source       TEXT    NOT NULL,  -- 'gateway' or 'hub'
        gateway_id   INTEGER,
        net_id       INTEGER,
        claimed_src  INTEGER,           -- NULL: a node the hub doesn't know, or lumped
        reason       TEXT    NOT NULL,
        severity     TEXT    NOT NULL,  -- 'info', 'warn', 'high' or 'critical'
        count        INTEGER NOT NULL,
        first_at     INTEGER NOT NULL,
        last_at      INTEGER NOT NULL,
        rssi_min     REAL,
        rssi_max     REAL,
        sample       BLOB,              -- the first dropped frame or message
        detail       TEXT               -- the hub's reason, for its own drops
    );
    CREATE INDEX rejections_time ON rejections(window_start);
    CREATE INDEX rejections_node ON rejections(net_id, claimed_src, window_start);
    """,
    """
    -- The header's "port" byte is now called the message schema (PROTOCOL.md §5).
    ALTER TABLE uplinks RENAME COLUMN port TO schema;
    ALTER TABLE nodes RENAME COLUMN last_port TO last_schema;
    """,
]

SCHEMA_VERSION = len(MIGRATIONS)


def connect_writer(path: str | Path) -> sqlite3.Connection:
    """The one read-write connection. Transactions are explicit (BEGIN/COMMIT)."""
    conn = sqlite3.connect(path, isolation_level=None)
    conn.execute("PRAGMA journal_mode = WAL")
    # In WAL mode NORMAL is safe against crashes of this process. A power cut
    # can lose the last few commits but never corrupts the file, and it saves
    # an fsync per commit, which matters on an SD card.
    conn.execute("PRAGMA synchronous = NORMAL")
    conn.execute("PRAGMA foreign_keys = ON")
    conn.execute("PRAGMA busy_timeout = 5000")
    return conn


def connect_reader(path: str | Path) -> sqlite3.Connection:
    """A read-only connection for any process other than the writer."""
    uri = Path(path).resolve().as_uri() + "?mode=ro"
    conn = sqlite3.connect(uri, uri=True)
    conn.execute("PRAGMA query_only = ON")
    conn.execute("PRAGMA busy_timeout = 5000")
    conn.row_factory = sqlite3.Row
    return conn


def migrate(conn: sqlite3.Connection) -> int:
    """Bring the schema up to date. Writer only. Returns the schema version."""
    version = conn.execute("PRAGMA user_version").fetchone()[0]
    if version > SCHEMA_VERSION:
        raise RuntimeError(
            f"database schema is version {version}, newer than this code ({SCHEMA_VERSION})"
        )
    for number in range(version + 1, SCHEMA_VERSION + 1):
        script = MIGRATIONS[number - 1]
        try:
            conn.executescript(
                f"BEGIN IMMEDIATE;\n{script}\nPRAGMA user_version = {number};\nCOMMIT;"
            )
        except sqlite3.Error:
            if conn.in_transaction:
                conn.execute("ROLLBACK")
            raise
    return SCHEMA_VERSION


PRUNE_CHUNK = 500  # uplinks or messages per transaction


def prune(conn: sqlite3.Connection, cutoff: int, max_chunks: int = 20) -> int:
    """Delete everything received before `cutoff` (unix ms). Writer only.

    Works in short transactions of PRUNE_CHUNK rows, so readers and the next
    batch don't wait long, and stops after max_chunks; whatever is left goes
    in the next round. Returns the number of uplinks and messages deleted.

    Order matters for the foreign keys: an uplink's readings and receptions
    go with it, and a message goes only once no reception points at it. A
    reception is never older than its uplink, so once every old uplink is
    gone, no old message has a reception left.
    """
    deleted = 0
    for _ in range(max_chunks):
        conn.execute("BEGIN IMMEDIATE")
        ids = [
            row[0]
            for row in conn.execute(
                "SELECT id FROM uplinks WHERE received_at < ? ORDER BY id LIMIT ?",
                (cutoff, PRUNE_CHUNK),
            )
        ]
        if ids:  # old uplinks first, with what hangs off them
            marks = ",".join("?" * len(ids))
            conn.execute(f"DELETE FROM readings WHERE uplink_id IN ({marks})", ids)
            conn.execute(f"DELETE FROM receptions WHERE uplink_id IN ({marks})", ids)
            conn.execute(f"DELETE FROM uplinks WHERE id IN ({marks})", ids)
            n = len(ids)
        else:  # then old messages
            n = conn.execute(
                "DELETE FROM messages WHERE id IN"
                " (SELECT id FROM messages WHERE received_at < ? ORDER BY id LIMIT ?)",
                (cutoff, PRUNE_CHUNK),
            ).rowcount
        if n == 0:  # finally, nodes not heard from since the cutoff, and old summaries
            conn.execute("DELETE FROM nodes WHERE last_seen < ?", (cutoff,))
            conn.execute("DELETE FROM rejections WHERE window_start < ?", (cutoff,))
        conn.execute("COMMIT")
        deleted += n
        if n == 0:
            break
    return deleted


def close_writer(conn: sqlite3.Connection) -> None:
    conn.execute("PRAGMA optimize")
    conn.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    conn.close()
