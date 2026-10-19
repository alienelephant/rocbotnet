"""The downlink process: services ask for downlink, the hub queues it, numbers
it per node, and hands each frame to one gateway (PROTOCOL.md §9.1,
GATEWAY_INTERFACE.md §5).

    service ──roclink/svc/<service>/down──► hub ──roclink/out/<net>/<gw>/down──► gateway
    service ◄─roclink/svc/<service>/down_status── hub ◄─roclink/in/<net>/<gw>/down_status── gateway

- **Who may send what:** a service may send to a node and schema its [[route]]s
  match, the same rule as for what it receives. The broker's ACL makes sure
  only that service publishes on its .../down topic.
- **Numbering:** one persisted downlink seq per node, starting at a random
  value. A re-offer keeps its seq, even through another gateway, so the node
  can't run it twice (node rule 4).
- **Choosing a gateway:** the one that most recently heard the node (this
  process follows the gateways' uplinks with its own MQTT session).
- **When:** a sleepy node's downlink is offered for its next listen window
  ("send": "window"); an always-listening node's is sent at once ("send":
  "now"), bound to the node's most recent secured uplink the hub has seen
  (PROTOCOL.md §10.6). A node the hub hasn't heard from yet waits.
- **Outcome:** the gateway reports acked, sent or expired. An expired offer
  is offered again, through whichever gateway heard the node last, until
  max_attempts or the request's time to live runs out. The service is told
  queued, then acked, sent or failed.

Everything lives in downlink.db, this process's own SQLite file, written
before anything is published.
"""

import json
import logging
import secrets
import sqlite3
import threading
from pathlib import Path

from . import broker, frame
from .process import child_init, now_ms
from .record import RecordError, gateway_from_topic, parse_uplink
from .registry import ALWAYS_LISTENING

log = logging.getLogger(__name__)

SCHEMA = """
CREATE TABLE IF NOT EXISTS node_seq (
    net_id INTEGER NOT NULL, node_id INTEGER NOT NULL, last_seq INTEGER NOT NULL,
    PRIMARY KEY (net_id, node_id)) WITHOUT ROWID;
-- The latest uplink heard from each node: which gateway, and its frame counter.
CREATE TABLE IF NOT EXISTS node_seen (
    net_id INTEGER NOT NULL, node_id INTEGER NOT NULL, gateway_id INTEGER NOT NULL,
    fcnt INTEGER, key_gen INTEGER, at INTEGER NOT NULL,
    PRIMARY KEY (net_id, node_id)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS downlinks (
    id         TEXT PRIMARY KEY,   -- the hub's id, sent to the gateway and the service
    ref        TEXT,               -- the service's own id for the request, echoed back
    service    TEXT NOT NULL,
    net_id     INTEGER NOT NULL,
    node_id    INTEGER NOT NULL,
    schema       INTEGER NOT NULL,
    payload    BLOB NOT NULL,
    ack        INTEGER NOT NULL,
    seq        INTEGER NOT NULL,
    state      TEXT NOT NULL,      -- 'waiting', 'offered', 'acked', 'sent', 'failed'
    created    INTEGER NOT NULL,
    expires    INTEGER NOT NULL,   -- give up after this
    attempts   INTEGER NOT NULL,
    gateway_id INTEGER,            -- of the current offer
    offer_until INTEGER            -- the current offer's deadline
);
CREATE INDEX IF NOT EXISTS downlinks_pending ON downlinks(state, net_id, node_id);
"""

FINAL = ("acked", "sent", "failed")


class DownlinkError(ValueError):
    pass


class Downlinks:
    """The queue and its rules. No MQTT here: `publish(topic, body)` is given,
    so it's easy to test."""

    def __init__(self, cfg, conn: sqlite3.Connection, publish, clock=now_ms):
        self.cfg = cfg
        self.d = cfg.downlink
        self.conn = conn
        self.publish = publish
        self.clock = clock
        self.lock = threading.Lock()  # paho's thread and the tick share the connection
        conn.executescript(SCHEMA)

    # ------------------------------------------------------------ requests

    def request(self, topic: str, raw: bytes) -> None:
        """A service's request on roclink/svc/<service>/down."""
        service = _service_from_topic(topic)
        ref = None
        try:
            rec = json.loads(raw)
            if not isinstance(rec, dict):
                raise DownlinkError("a request must be a JSON object")
            ref = rec.get("ref") if isinstance(rec.get("ref"), (str, int)) else None
            req = self._validate(service, rec)
        except (DownlinkError, RecordError, UnicodeDecodeError, json.JSONDecodeError) as e:
            self._tell(service, {"ref": ref, "status": "refused", "detail": str(e)})
            return
        with self.lock:
            dl_id = secrets.token_hex(8)
            now = self.clock()
            seq = self._next_seq(req["net"], req["node"])
            self.conn.execute(
                "INSERT INTO downlinks (id, ref, service, net_id, node_id, schema, payload, ack, seq,"
                " state, created, expires, attempts) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 'waiting', ?, ?, 0)",
                (dl_id, None if ref is None else str(ref), service, req["net"], req["node"],
                 req["schema"], req["payload"], int(req["ack"]), seq, now,
                 now + req["ttl_s"] * 1000),
            )
            self._tell(service, {"ref": ref, "id": dl_id, "status": "queued", "seq": seq})
            self._offer(dl_id, now)

    def _validate(self, service: str | None, rec: dict) -> dict:
        if service is None:
            raise DownlinkError("not a service topic")
        try:
            net = _int(rec, "net", 0, 0xFF)
            node = _int(rec, "node", frame.NODE_MIN, frame.NODE_MAX)
            schema = _int(rec, "schema", 1, 0xFF)
            payload = bytes.fromhex(rec.get("payload", ""))
        except (ValueError, TypeError) as e:
            raise DownlinkError(str(e)) from None
        if net in self.cfg.public.nets:
            raise DownlinkError("no downlink on a public net")
        device_type = self.cfg.registry.device_type(net, node)
        type_name = device_type.name if device_type is not None else None
        if not any(r.service == service and r.matches(net, node, schema, type_name)
                   for r in self.cfg.routes):
            raise DownlinkError(f"{service} has no route to node 0x{node:04X} schema {schema}")
        ttl_s = rec.get("ttl_s", self.d.ttl_s)
        if not isinstance(ttl_s, int) or not 1 <= ttl_s <= self.d.ttl_s:
            raise DownlinkError(f"ttl_s must be 1..{self.d.ttl_s}")
        ack = rec.get("ack", True)
        if not isinstance(ack, bool):
            raise DownlinkError("ack must be true or false")
        return {"net": net, "node": node, "schema": schema, "payload": payload, "ack": ack,
                "ttl_s": ttl_s}

    def _next_seq(self, net: int, node: int) -> int:
        row = self.conn.execute(
            "SELECT last_seq FROM node_seq WHERE net_id = ? AND node_id = ?", (net, node)
        ).fetchone()
        seq = (row[0] + 1) % 256 if row else secrets.randbelow(256)  # random start (rule 3)
        self.conn.execute(
            "INSERT INTO node_seq (net_id, node_id, last_seq) VALUES (?, ?, ?)"
            " ON CONFLICT (net_id, node_id) DO UPDATE SET last_seq = excluded.last_seq",
            (net, node, seq),
        )
        return seq

    # ------------------------------------------------------------- offers

    def _offer(self, dl_id: str, now: int) -> None:
        """Offer a downlink through the gateway that heard its node last, if any."""
        row = self.conn.execute(
            "SELECT net_id, node_id, schema, payload, ack, seq, expires, attempts, service, ref"
            " FROM downlinks WHERE id = ?", (dl_id,)
        ).fetchone()
        net, node, schema, payload, ack, seq, expires, attempts, service, ref = row
        if now >= expires or attempts >= self.d.max_attempts:
            self._finish(dl_id, service, ref, "failed", "expired before the node answered")
            return
        seen = self.conn.execute(
            "SELECT gateway_id, fcnt FROM node_seen WHERE net_id = ? AND node_id = ?", (net, node)
        ).fetchone()
        if seen is None:  # never heard from: wait for its first uplink
            self.conn.execute(
                "UPDATE downlinks SET state = 'waiting' WHERE id = ?", (dl_id,))
            return
        gateway, fcnt = seen
        now_send = self.cfg.registry.node_class(net, node) == ALWAYS_LISTENING
        until = min(expires, now + 1000 * (self.d.offer_now_s if now_send else self.d.offer_window_s))
        self.conn.execute(
            "UPDATE downlinks SET state = 'offered', attempts = attempts + 1, gateway_id = ?,"
            " offer_until = ? WHERE id = ?",
            (gateway, until, dl_id),
        )
        body = {
            "v": 1, "id": dl_id, "node": f"0x{node:04X}", "schema": schema, "seq": seq,
            "ack": bool(ack), "payload": payload.hex(),
            "send": "now" if now_send else "window",
            "binding": fcnt if now_send else None,
            "expires": until,
        }
        self.publish(self.d.out_topic.format(net=net, gateway=gateway), _json(body))

    # ------------------------------------------------- uplinks and statuses

    def uplink(self, topic: str, raw: bytes) -> None:
        """A gateway's uplink: remember who heard the node, and offer what waits for it."""
        try:
            up = parse_uplink(topic, raw)
        except (RecordError, frame.FrameError):
            return  # ingest records it
        if up.gateway_id is None or up.net_id in self.cfg.public.nets:
            return
        with self.lock:
            now = self.clock()
            self.conn.execute(
                "INSERT INTO node_seen (net_id, node_id, gateway_id, fcnt, key_gen, at)"
                " VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT (net_id, node_id) DO UPDATE SET"
                " gateway_id = excluded.gateway_id, at = excluded.at,"
                " fcnt = CASE WHEN excluded.fcnt IS NULL OR node_seen.fcnt IS NULL"
                "   OR excluded.key_gen != node_seen.key_gen OR excluded.fcnt > node_seen.fcnt"
                "   THEN excluded.fcnt ELSE node_seen.fcnt END,"
                " key_gen = excluded.key_gen",
                (up.net_id, up.node_id, up.gateway_id, up.fcnt, up.key_gen, now),
            )
            waiting = self.conn.execute(
                "SELECT id FROM downlinks WHERE state = 'waiting' AND net_id = ? AND node_id = ?"
                " ORDER BY created", (up.net_id, up.node_id)
            ).fetchall()
            for (dl_id,) in waiting:
                self._offer(dl_id, now)

    def status(self, topic: str, raw: bytes) -> None:
        """A gateway's down_status: acked, sent or expired."""
        try:
            rec = json.loads(raw)
            dl_id, status = rec["id"], rec["status"]
        except (UnicodeDecodeError, json.JSONDecodeError, KeyError, TypeError):
            log.warning("bad down_status on %s", topic)
            return
        with self.lock:
            row = self.conn.execute(
                "SELECT state, gateway_id, service, ref FROM downlinks WHERE id = ?", (str(dl_id),)
            ).fetchone()
            if row is None or row[0] != "offered" or row[1] != gateway_from_topic(topic):
                return  # unknown, finished, or from a gateway that isn't offering it
            _, _, service, ref = row
            if status in ("acked", "sent"):
                self._finish(str(dl_id), service, ref, status)
            elif status == "expired":
                self._offer(str(dl_id), self.clock())

    def tick(self) -> None:
        """Offers whose gateway never answered count as expired."""
        with self.lock:
            now = self.clock()
            late = self.conn.execute(
                "SELECT id FROM downlinks WHERE state = 'offered' AND offer_until < ?",
                (now - 5000,),  # a little grace for the gateway's own report
            ).fetchall()
            for (dl_id,) in late:
                self._offer(dl_id, now)

    def _finish(self, dl_id: str, service: str, ref, state: str, detail: str | None = None):
        self.conn.execute("UPDATE downlinks SET state = ? WHERE id = ?", (state, dl_id))
        msg = {"ref": ref, "id": dl_id, "status": state}
        if detail:
            msg["detail"] = detail
        self._tell(service, msg)

    def _tell(self, service: str | None, msg: dict) -> None:
        if service is not None:
            msg = {k: v for k, v in msg.items() if v is not None}
            self.publish(self.d.service_status_topic.format(service=service), _json(msg))


def run(cfg, stop) -> None:
    parent_gone = child_init(cfg.log_level)
    conn = sqlite3.connect(Path(cfg.state_path("downlink.db")), isolation_level=None,
                           check_same_thread=False)
    conn.execute("PRAGMA journal_mode = WAL")
    conn.execute("PRAGMA synchronous = NORMAL")
    holder = {}
    d = cfg.downlink

    def publish(topic, body):
        holder["client"].publish(topic, body, qos=cfg.mqtt.qos)

    queue = Downlinks(cfg, conn, publish)

    def on_message(client, userdata, msg):
        try:
            if msg.topic.endswith("/down_status"):
                queue.status(msg.topic, msg.payload)
            elif msg.topic.endswith("/down"):
                queue.request(msg.topic, msg.payload)
            else:
                queue.uplink(msg.topic, msg.payload)
        except Exception:  # a bug: log it, and keep paho's thread alive
            log.exception("could not handle message on %s", msg.topic)

    topics = [d.request_topic, d.status_topic] + cfg.mqtt.topics
    holder["client"] = broker.connect(cfg.mqtt, f"{cfg.mqtt.client_id}-downlink", topics, on_message)
    broker.wait_then_disconnect(holder["client"], stop, parent_gone, tick=lambda final: queue.tick())
    conn.close()
    log.info("stopped")


def _service_from_topic(topic: str) -> str | None:
    parts = topic.split("/")  # roclink/svc/<service>/down
    return parts[2] if len(parts) == 4 and parts[1] == "svc" and parts[3] == "down" else None


def _int(rec: dict, key: str, lo: int, hi: int) -> int:
    value = rec.get(key)
    if isinstance(value, str):
        value = int(value, 0)
    if isinstance(value, bool) or not isinstance(value, int):
        raise DownlinkError(f"{key} must be an integer")
    if not lo <= value <= hi:
        raise DownlinkError(f"{key} {value} is outside {lo}..{hi}")
    return value


def _json(obj) -> bytes:
    return json.dumps(obj, separators=(",", ":")).encode()

