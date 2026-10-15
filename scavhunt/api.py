"""Read-only HTTP: the scoreboard and its JSON (docs/GAME.md §8).

    GET /                    a scoreboard page that refreshes itself
    GET /api/run             the current run: state, times, boxes, teams' progress
    GET /api/standings       the current run's standings
    GET /api/events?after=N  the current run's log, from event N on

Each request reads the log afresh, so this can run in its own process, and
nothing here can change the game.
"""

from __future__ import annotations

import json
import logging
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from importlib.resources import files
from urllib.parse import parse_qs, urlparse

from .game import Game
from .store import Store

log = logging.getLogger(__name__)


def run_view(game: Game, store: Store) -> dict:
    rid = store.current_run_id(game)
    boxes = [
        {"id": b.id, "name": b.name, "requires": list(b.requires), "required": b.required}
        for b in game.boxes.values()
    ]
    view = {"game": game.id, "title": game.title, "boxes": boxes, "run": None}
    if rid is None:
        return view
    run = store.load(game, rid)
    view["run"] = {
        "id": run.id,
        "state": run.state,
        "started_at": run.started_at,
        "deadline": run.deadline,
        "ended_at": run.ended_at,
        "winner": run.winner(),
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


def standings(game: Game, store: Store) -> list[dict]:
    rid = store.current_run_id(game)
    return store.load(game, rid).standings() if rid else []


def serve(game: Game, db_path: str, host: str, port: int) -> None:
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
