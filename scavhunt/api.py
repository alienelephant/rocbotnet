"""Read-only HTTP: the scoreboard and its JSON (docs/GAME.md §8).

    GET /                    a scoreboard page that refreshes itself
    GET /api/run             the current run: state, times, boxes, teams' progress
    GET /api/standings       the current run's standings
    GET /api/events?after=N  the current run's log, from event N on
    GET /api/box_data        each box's own data (schemas 32-255), decoded by the hub:
                             the latest, and the most recent few. Read from the
                             hub's API ([api] hub_api), if set

Each request reads the log afresh, so this can run in its own process, and
nothing here can change the game.
"""

from __future__ import annotations

import json
import logging
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from importlib.resources import files
from urllib.parse import parse_qs, urlparse

from .game import Game
from .service import now_ms as s_now
from .store import Store

log = logging.getLogger(__name__)


def run_view(game: Game, store: Store, now: int | None = None) -> dict:
    rid = store.current_run_id(game)
    boxes = [
        {"id": b.id, "name": b.name, "net": b.net, "node": b.node, "requires": list(b.requires),
         "required": b.required, "points": b.points, "hints": len(b.hint_penalties_s),
         "hint_penalties_s": list(b.hint_penalties_s), "wrong_penalty_s": b.wrong_penalty_s}
        for b in game.boxes.values()
    ]
    view = {"game": game.id, "title": game.title, "boxes": boxes, "run": None}
    if rid is None:
        return view
    run = store.load(game, rid)
    now = now if now is not None else s_now()
    view["run"] = {
        "id": run.id,
        "state": run.state,
        "started_at": run.started_at,
        "deadline": run.deadline,
        "ended_at": run.ended_at,
        "winner": run.winner(),
        "session_timeout_s": game.session_timeout_s,
        "sessions": {
            bid: {"team": s.team, "start": s.start, "last": s.last}
            for bid in game.boxes
            if (s := run.session_at(bid, now)) is not None
        },
        "teams": [
            {
                "team": t.id,
                "name": t.name,
                "solved": t.solved,
                "open": [b.id for b in game.boxes.values()
                         if b.id not in t.solved and run.is_open_for(t, b)],
                "finished_at": t.finished_at,
            }
            for t in run.teams.values()
        ],
    }
    return view


def standings(game: Game, store: Store, now: int | None = None) -> list[dict]:
    rid = store.current_run_id(game)
    if not rid:
        return []
    run = store.load(game, rid)
    if now is None:
        now = run.ended_at if run.state == "ended" else s_now()
    return run.standings(now)


FIRST_BOX_SCHEMA = 32  # below: the game's own messages (GAME.md §6)


BOX_DATA_RECENT = 10  # messages per box, for its log on the scoreboard


def box_data(game: Game, hub_api: str, get=None) -> dict:
    """Each box's own data (schema 32 and up) from the hub's read-only API, as
    the hub decoded it: the latest message's fields, and `recent`, its last few
    messages, newest first. A box with none is left out."""
    get = get or (lambda url: json.loads(urllib.request.urlopen(url, timeout=2).read()))
    fields = ("received_at", "schema", "payload", "decoded", "decode_error")
    out = {}
    for b in game.boxes.values():
        ups = get(f"{hub_api.rstrip('/')}/api/uplinks?net={b.net}&node={b.node}"
                  f"&min_schema={FIRST_BOX_SCHEMA}&limit={BOX_DATA_RECENT}")
        recent = [{k: u.get(k) for k in fields} for u in ups if u["schema"] >= FIRST_BOX_SCHEMA]
        if recent:
            out[b.id] = {**recent[0], "recent": recent}
    return out


class _Cached:
    """box_data(), at most once every `ttl_s`: every open page polls it."""

    def __init__(self, fn, ttl_s: float = 2.0):
        self.fn, self.ttl_s = fn, ttl_s
        self.lock = threading.Lock()
        self.at, self.value = 0.0, None

    def __call__(self):
        with self.lock:
            if self.value is None or time.monotonic() - self.at >= self.ttl_s:
                try:
                    self.value = self.fn()
                except (OSError, ValueError, KeyError) as e:
                    log.warning("box data from the hub: %s", e)
                    self.value = {"error": f"the hub's API didn't answer: {e}"}
                self.at = time.monotonic()
            return self.value


def serve(game: Game, db_path: str, host: str, port: int, hub_api: str | None = None) -> None:
    cached_box_data = _Cached(lambda: box_data(game, hub_api)) if hub_api else (lambda: {})

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            url = urlparse(self.path)
            store = Store(db_path)
            try:
                if url.path == "/":
                    page = files("scavhunt").joinpath("scoreboard.html").read_bytes()
                    return self._send(200, page, "text/html; charset=utf-8")
                if url.path == "/api/run":
                    return self._json(200, run_view(game, store))
                if url.path == "/api/standings":
                    return self._json(200, standings(game, store))
                if url.path == "/api/events":
                    rid = store.current_run_id(game)
                    after = int(parse_qs(url.query).get("after", ["0"])[0])
                    return self._json(200, store.events(rid, after) if rid else [])
                if url.path == "/api/box_data":
                    return self._json(200, cached_box_data())
                self._json(404, {"error": "not found"})
            except ValueError as e:
                self._json(400, {"error": str(e)})
            finally:
                store.close()

        def _json(self, status, obj):
            self._send(status, json.dumps(obj).encode(), "application/json")

        def _send(self, status, body, ctype):
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, fmt, *args):
            log.debug("%s " + fmt, self.address_string(), *args)

    server = ThreadingHTTPServer((host, port), Handler)
    log.info("scoreboard on http://%s:%d/", host, port)
    try:
        server.serve_forever()
    finally:
        server.server_close()
