"""An API process: a small read-only JSON API over one database.

It's an example reader and a starting point for a web UI. Each request opens
its own read-only connection, so a request never waits for the writer and
can never write.

Two can run: one over hub.db ([api], localhost by default) and one over
public.db ([public.api]) for the public page. The public one is a separate
process that is only ever given public.db, so nothing it serves can come
from hub.db.

    GET /api/health                          schema version, message counts
    GET /api/nodes                           every node, most recent first
    GET /api/uplinks?node=&net=&schema=&min_schema=&limit=&before=
    GET /api/readings?node=&metric=&net=&since=&limit=
    GET /api/rejections?severity=&reason=&net=&node=&since=&limit=

Ids may be decimal or 0x-hex. `since` and `before` take unix ms and uplink
ids respectively. `min_schema` keeps schemas from that one up (a device's own
messages, say, without its PINGs on schema 0). For rejections, `severity` means "at least" (warn = warn,
high and critical) and `node` matches the claimed src.
"""

import json
import logging
import threading
from contextlib import closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

from . import db
from .monitor import LEVELS
from .process import child_init, now_ms

log = logging.getLogger(__name__)

MAX_LIMIT = 10_000


def run(cfg, db_path: str, api_cfg, stop) -> None:
    parent_gone = child_init(cfg.log_level)
    server = ThreadingHTTPServer((api_cfg.host, api_cfg.port), Handler)
    server.daemon_threads = True
    server.db_path = db_path
    thread = threading.Thread(target=server.serve_forever, name="http")
    thread.start()
    log.info("serving %s on http://%s:%d", db_path, api_cfg.host, api_cfg.port)

    while not stop.wait(1.0):
        if parent_gone():
            log.error("supervisor is gone, stopping")
            break

    server.shutdown()
    thread.join()
    server.server_close()
    log.info("stopped")


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        url = urlsplit(self.path)
        route = ROUTES.get(url.path)
        if route is None:
            return self._send(404, {"error": "not found"})
        params = {k: v[-1] for k, v in parse_qs(url.query).items()}
        try:
            with closing(db.connect_reader(self.server.db_path)) as conn:
                body = route(conn, params)
        except ValueError as e:
            return self._send(400, {"error": str(e)})
        self._send(200, body)

    def _send(self, status: int, body) -> None:
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        log.debug("%s %s", self.address_string(), fmt % args)


def health(conn, params):
    hour_ago = now_ms() - 3_600_000
    counts = conn.execute(
        "SELECT status, COUNT(*) FROM messages WHERE received_at >= ? GROUP BY status",
        (hour_ago,),
    ).fetchall()
    last = conn.execute("SELECT MAX(received_at) FROM messages").fetchone()[0]
    rejected = conn.execute(
        "SELECT severity, SUM(count) FROM rejections WHERE window_start >= ? GROUP BY severity",
        (hour_ago,),
    ).fetchall()
    return {
        "schema": conn.execute("PRAGMA user_version").fetchone()[0],
        "last_message_at": last,
        "messages_last_hour": {status: n for status, n in counts},
        "rejections_last_hour": {severity: n for severity, n in rejected},
    }


def nodes(conn, params):
    rows = conn.execute("SELECT * FROM nodes ORDER BY last_seen DESC").fetchall()
    return [dict(r) for r in rows]


def uplinks(conn, params):
    where, args = _filters(params, ("net", "net_id"), ("node", "node_id"), ("schema", "schema"))
    if "before" in params:
        where.append("id < ?")
        args.append(_id(params, "before"))
    if "min_schema" in params:
        where.append("schema >= ?")
        args.append(_id(params, "min_schema"))
    rows = conn.execute(
        "SELECT u.*, (SELECT COUNT(*) FROM receptions r WHERE r.uplink_id = u.id) AS receptions"
        f" FROM uplinks u {_where(where)} ORDER BY id DESC LIMIT ?",
        (*args, _limit(params, 50)),
    ).fetchall()
    out = []
    for r in rows:
        row = dict(r)
        row["payload"] = row["payload"].hex()
        row["decoded"] = json.loads(row["decoded"]) if row["decoded"] else None
        out.append(row)
    return out


def readings(conn, params):
    if "node" not in params or "metric" not in params:
        raise ValueError("node and metric are required")
    where, args = _filters(params, ("net", "net_id"), ("node", "node_id"))
    where.append("metric = ?")
    args.append(params["metric"])
    if "since" in params:
        where.append("received_at >= ?")
        args.append(_id(params, "since"))
    # Newest first for the LIMIT, then oldest first for plotting.
    rows = conn.execute(
        f"SELECT received_at, value FROM readings {_where(where)}"
        " ORDER BY received_at DESC LIMIT ?",
        (*args, _limit(params, 1000)),
    ).fetchall()
    return [[r["received_at"], r["value"]] for r in reversed(rows)]


def rejections(conn, params):
    where, args = _filters(params, ("net", "net_id"), ("node", "claimed_src"))
    if "severity" in params:
        if params["severity"] not in LEVELS:
            raise ValueError(f"severity must be one of {', '.join(LEVELS)}")
        levels = LEVELS[LEVELS.index(params["severity"]) :]
        where.append(f"severity IN ({','.join('?' * len(levels))})")
        args.extend(levels)
    if "reason" in params:
        where.append("reason = ?")
        args.append(params["reason"])
    if "since" in params:
        where.append("window_start >= ?")
        args.append(_id(params, "since"))
    rows = conn.execute(
        f"SELECT * FROM rejections {_where(where)} ORDER BY window_start DESC, id DESC LIMIT ?",
        (*args, _limit(params, 100)),
    ).fetchall()
    out = []
    for r in rows:
        row = dict(r)
        row["sample"] = row["sample"].hex() if row["sample"] is not None else None
        out.append(row)
    return out


ROUTES = {
    "/api/health": health,
    "/api/nodes": nodes,
    "/api/uplinks": uplinks,
    "/api/readings": readings,
    "/api/rejections": rejections,
}


def _filters(params, *names):
    where, args = [], []
    for param, column in names:
        if param in params:
            where.append(f"{column} = ?")
            args.append(_id(params, param))
    return where, args


def _where(clauses) -> str:
    return "WHERE " + " AND ".join(clauses) if clauses else ""


def _id(params, key) -> int:
    try:
        return int(params[key], 0)
    except ValueError:
        raise ValueError(f"{key} must be an integer") from None


def _limit(params, default) -> int:
    limit = _id(params, "limit") if "limit" in params else default
    if not 1 <= limit <= MAX_LIMIT:
        raise ValueError(f"limit must be 1..{MAX_LIMIT}")
    return limit
