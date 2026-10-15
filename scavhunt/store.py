"""The event log (docs/GAME.md §8): every run and every change to one, in
order, in SQLite. Nothing is ever updated or deleted; a run's state is its
events replayed through Run.apply().

The service and the operator's commands both write here, possibly at once.
Each change happens inside `change()`, which takes SQLite's write lock,
catches the run up with any events another process added, applies the new
event (refusing it if the rules do) and only then stores it.
"""

from __future__ import annotations

import json
import sqlite3
from contextlib import contextmanager

from .game import Game, GameError, Run

SCHEMA = """
CREATE TABLE IF NOT EXISTS runs (
    id      TEXT PRIMARY KEY,
    game_id TEXT NOT NULL,
    created INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS events (
    id     INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id TEXT NOT NULL REFERENCES runs (id),
    at     INTEGER NOT NULL,          -- hub time, ms since the epoch
    kind   TEXT NOT NULL,
    body   TEXT NOT NULL,             -- the whole event, JSON
    uplink TEXT UNIQUE                -- "net:src:key_gen:fcnt" of the uplink it came from
);
CREATE INDEX IF NOT EXISTS events_run ON events (run_id, id);
"""


class Store:
    def __init__(self, path: str):
        self.conn = sqlite3.connect(path, isolation_level=None, check_same_thread=False)
        self.conn.execute("PRAGMA journal_mode = WAL")
        self.conn.execute("PRAGMA busy_timeout = 5000")
        self.conn.executescript(SCHEMA)

    def close(self) -> None:
        self.conn.close()

    # ----------------------------------------------------------- runs

    def create_run(self, game: Game, run_id: str, at: int) -> Run:
        try:
            self.conn.execute(
                "INSERT INTO runs (id, game_id, created) VALUES (?, ?, ?)", (run_id, game.id, at)
            )
        except sqlite3.IntegrityError:
            raise GameError(f"run {run_id} already exists") from None
        return Run(game, run_id)

    def current_run_id(self, game: Game) -> str | None:
        row = self.conn.execute(
            "SELECT id FROM runs WHERE game_id = ? ORDER BY created DESC, rowid DESC LIMIT 1",
            (game.id,),
        ).fetchone()
        return row[0] if row else None

    def runs(self, game: Game) -> list[dict]:
        rows = self.conn.execute(
            "SELECT id, created FROM runs WHERE game_id = ? ORDER BY created", (game.id,)
        )
        return [{"id": r[0], "created": r[1]} for r in rows]

    def load(self, game: Game, run_id: str) -> Run:
        row = self.conn.execute("SELECT game_id FROM runs WHERE id = ?", (run_id,)).fetchone()
        if row is None:
            raise GameError(f"no run {run_id}")
        if row[0] != game.id:
            raise GameError(f"run {run_id} is a run of {row[0]}, not {game.id}")
        run = Run(game, run_id)
        self.catch_up(run)
        return run

    # --------------------------------------------------------- events

    def catch_up(self, run: Run) -> None:
        """Apply events added since this Run object last saw the log."""
        rows = self.conn.execute(
            "SELECT id, body FROM events WHERE run_id = ? AND id > ? ORDER BY id",
            (run.id, run.last_event),
        ).fetchall()
        for eid, body in rows:
            run.apply(json.loads(body))
            run.last_event = eid

    def events(self, run_id: str, after: int = 0, limit: int = 500) -> list[dict]:
        rows = self.conn.execute(
            "SELECT id, body FROM events WHERE run_id = ? AND id > ? ORDER BY id LIMIT ?",
            (run_id, after, limit),
        )
        return [{"id": eid, **json.loads(body)} for eid, body in rows]

    def handled(self, uplink: str) -> bool:
        return self.conn.execute("SELECT 1 FROM events WHERE uplink = ?", (uplink,)).fetchone() is not None

    @contextmanager
    def change(self, run: Run):
        """Hold the write lock with `run` up to date; yields an append(event,
        uplink=None) function. Everything appended is applied, then stored."""
        self.conn.execute("BEGIN IMMEDIATE")
        try:
            self.catch_up(run)

            def append(ev: dict, uplink: str | None = None) -> int:
                run.apply(ev)  # raises GameError before anything is stored
                cur = self.conn.execute(
                    "INSERT INTO events (run_id, at, kind, body, uplink) VALUES (?, ?, ?, ?, ?)",
                    (run.id, ev["at"], ev["kind"], json.dumps(ev, separators=(",", ":")), uplink),
                )
                run.last_event = cur.lastrowid
                return cur.lastrowid

            yield append
        except BaseException:
            self.conn.execute("ROLLBACK")
            # The Run may have applied an event that wasn't stored: rebuild it.
            fresh = self.load(run.game, run.id)
            run.__dict__.update(fresh.__dict__)
            raise
        else:
            self.conn.execute("COMMIT")
