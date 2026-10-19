"""The hub's anti-replay window (PROTOCOL.md §9.1).

Each gateway refuses a frame counter it has already accepted, but only its
own: a secured frame recorded near one gateway and replayed to another that
never heard it passes that gateway's check. The hub sees every gateway, so it
keeps, per (net, node, key_gen), the highest frame counter it has accepted
and which of the WINDOW counters just below it it has seen, as IPsec does
(RFC 4303 §3.4.3):

    above the highest           new: deliver, and slide the window up
    in the window, not seen     new: gateways can forward one node's frames out of order
    in the window, seen         duplicate: another gateway heard the same frame
    below the window            replay: drop, and count it as a "replay" drop

The window is persisted, one SQLite row per node, written whenever a new frame
is accepted, so a restart doesn't reopen it. Each process that needs it
(ingest, before storing; the router, before delivering) keeps its own file:
they see the same frames, so they reach the same verdicts.
"""

import sqlite3
from pathlib import Path

WINDOW = 64
NEW, DUPLICATE, REPLAY = "new", "duplicate", "replay"
_MASK = (1 << WINDOW) - 1


def classify(highest: int | None, seen: int, fcnt: int) -> tuple[str, int, int]:
    """One step of the window. `seen` has bit i set when highest - i was seen.
    Returns (verdict, new highest, new seen)."""
    if highest is None:
        return NEW, fcnt, 1
    if fcnt > highest:
        shift = fcnt - highest
        seen = ((seen << shift) | 1) & _MASK if shift < WINDOW else 1
        return NEW, fcnt, seen
    behind = highest - fcnt
    if behind >= WINDOW:
        return REPLAY, highest, seen
    bit = 1 << behind
    if seen & bit:
        return DUPLICATE, highest, seen
    return NEW, highest, seen | bit


class ReplayGuard:
    """The windows of every secured node, persisted in their own SQLite file."""

    def __init__(self, path: str | Path):
        self.conn = sqlite3.connect(path, isolation_level=None, check_same_thread=False)
        self.conn.execute("PRAGMA journal_mode = WAL")
        self.conn.execute("PRAGMA synchronous = NORMAL")
        self.conn.execute(
            "CREATE TABLE IF NOT EXISTS replay_windows ("
            " net_id INTEGER NOT NULL, node_id INTEGER NOT NULL, key_gen INTEGER NOT NULL,"
            " highest INTEGER NOT NULL, seen INTEGER NOT NULL,"
            " PRIMARY KEY (net_id, node_id, key_gen)) WITHOUT ROWID"
        )
        self.windows: dict[tuple[int, int, int], tuple[int, int]] = {
            (net, node, gen): (highest, _unsigned(seen))
            for net, node, gen, highest, seen in self.conn.execute(
                "SELECT net_id, node_id, key_gen, highest, seen FROM replay_windows"
            )
        }

    def check(self, net_id: int, node_id: int, key_gen: int, fcnt: int) -> str:
        """The verdict for a secured uplink. A new one is recorded, and persisted
        before this returns."""
        key = (net_id, node_id, key_gen)
        highest, seen = self.windows.get(key, (None, 0))
        verdict, highest, seen = classify(highest, seen, fcnt)
        if verdict == NEW:
            self.windows[key] = (highest, seen)
            self.conn.execute(
                "INSERT INTO replay_windows (net_id, node_id, key_gen, highest, seen)"
                " VALUES (?, ?, ?, ?, ?) ON CONFLICT (net_id, node_id, key_gen)"
                " DO UPDATE SET highest = excluded.highest, seen = excluded.seen",
                (net_id, node_id, key_gen, highest, _signed(seen)),
            )
        return verdict

    def close(self) -> None:
        self.conn.close()


# SQLite integers are signed 64-bit; the bitmap uses all 64 bits.
def _signed(v: int) -> int:
    return v - (1 << 64) if v >= 1 << 63 else v


def _unsigned(v: int) -> int:
    return v + (1 << 64) if v < 0 else v
