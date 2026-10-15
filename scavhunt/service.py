"""The game as a roclink hub service (docs/GAME.md §8).

The hub delivers each box's uplink to roclink/svc/<service>/up, already
verified, decrypted and de-duplicated across gateways. For a WIN, the service
judges the claim, stores it, and asks the hub to send the box its VERDICT on
roclink/svc/<service>/down. The hub answers on .../down_status.

`Service` holds the logic and returns what to publish; `run()` wires it to
MQTT. Operator commands (__main__.py) write to the same log and the service
picks them up, so it never needs restarting during a game.
"""

from __future__ import annotations

import json
import logging
import ssl
import threading
import time
from dataclasses import dataclass

from . import messages as msg
from .game import Game, GameError, Run, Verdict
from .store import Store

log = logging.getLogger(__name__)


def now_ms() -> int:
    return time.time_ns() // 1_000_000


@dataclass(frozen=True)
class ServiceConfig:
    name: str = "scavhunt"  # the hub's [[route]] service name, and the broker username
    host: str = "localhost"
    port: int = 1883
    username: str | None = None
    password: str | None = None
    tls: bool = False
    ca_file: str | None = None
    qos: int = 1
    verdict_ttl_s: int = 60  # a verdict older than this is no use to the team at the box
    allow_insecure: bool = False  # accept WINs below level "enc": bench testing only

    @property
    def up_topic(self) -> str:
        return f"roclink/svc/{self.name}/up"

    @property
    def down_topic(self) -> str:
        return f"roclink/svc/{self.name}/down"

    @property
    def status_topic(self) -> str:
        return f"roclink/svc/{self.name}/down_status"


class Service:
    """Turns delivered uplinks into game events and verdict downlinks. No MQTT here."""

    def __init__(self, game: Game, store: Store, cfg: ServiceConfig, clock=now_ms):
        self.game = game
        self.store = store
        self.cfg = cfg
        self.clock = clock
        self.run: Run | None = None
        self.failed_verdicts = 0
        self.refresh()

    def refresh(self) -> None:
        """Follow the operator: switch to the newest run, and catch up with its log."""
        rid = self.store.current_run_id(self.game)
        if rid is None:
            self.run = None
        elif self.run is None or self.run.id != rid:
            self.run = self.store.load(self.game, rid)
            log.info("following run %s (%s)", rid, self.run.state)
        else:
            self.store.catch_up(self.run)

    # --------------------------------------------------------------- uplink

    def uplink(self, raw: bytes) -> list[tuple[str, bytes]]:
        """One delivered record; returns the (topic, body) pairs to publish."""
        try:
            rec = json.loads(raw)
            net, src, schema = int(rec["net"]), int(rec["src"]), int(rec["schema"])
            payload = bytes.fromhex(rec["payload"])
            at = int(rec["hub_time"])
        except (ValueError, KeyError, TypeError) as e:
            log.warning("ignoring a record that doesn't parse: %s", e)
            return []
        box = self.game.box_at(net, src)
        if box is None:
            log.warning("uplink from 0x%02X/0x%04X, which isn't a box in %s", net, src, self.game.id)
            return []
        if schema not in (msg.SCHEMA_WIN, msg.SCHEMA_ATTEMPT):
            return []  # a box's own telemetry, say: not the game's business
        if rec.get("sec") != "enc" and not self.cfg.allow_insecure:
            log.warning("refusing %s from box %s: security %s, not enc", schema, box.id, rec.get("sec"))
            return []
        try:
            token = msg.parse_token_message(payload)
        except msg.MessageError as e:
            log.warning("bad message from box %s: %s", box.id, e)
            return []

        # The hub delivers at least once. A secured frame's counter makes each
        # uplink unique; an unsecured one (bench only) can't be de-duplicated.
        uplink = None
        if "fcnt" in rec:
            uplink = f"{net}:{src}:{rec.get('key_gen', 0)}:{rec['fcnt']}"
            if self.store.handled(uplink):
                log.info("repeat of %s, already handled", uplink)
                return []

        self.refresh()
        if self.run is None:
            log.warning("WIN at box %s, but no run has been created", box.id)
            return self._verdict_down(box, Verdict.NOT_RUNNING, 0, 0, None, None) if schema == msg.SCHEMA_WIN else []

        with self.store.change(self.run) as append:
            if schema == msg.SCHEMA_ATTEMPT:
                append({"kind": "attempt", "at": at, "box": box.id, "token": token.hex(),
                        "team": self.run.tokens.get(token)}, uplink)
                return []
            r = self.run.judge(token, box, at)
            eid = append({"kind": "claim", "at": at, "box": box.id, "token": token.hex(),
                          "team": r.team, "verdict": int(r.verdict)}, uplink)
        log.info("box %s, team %s: %s (%d of %d)", box.id, r.team or "?", r.verdict.name.lower(),
                 r.solved_required, r.required_total)
        return self._verdict_down(box, r.verdict, r.solved_required, r.required_total, r.needs, eid)

    def _verdict_down(self, box, verdict, solved, total, needs, eid) -> list[tuple[str, bytes]]:
        needs_node = self.game.boxes[needs].node if needs else 0
        body = {
            "net": box.net,
            "node": f"0x{box.node:04X}",
            "schema": msg.SCHEMA_VERDICT,
            "payload": msg.verdict(verdict, solved, total, needs_node).hex(),
            "ack": True,
            "ttl_s": self.cfg.verdict_ttl_s,
            "ref": f"{self.run.id}:{eid}" if self.run and eid else f"verdict:{box.id}",
        }
        return [(self.cfg.down_topic, _json(body))]

    # ------------------------------------------------------------- control

    def control(self, box_id: str, command: int) -> tuple[str, bytes]:
        box = self.game.boxes.get(box_id)
        if box is None:
            raise GameError(f"no box {box_id}")
        body = {"net": box.net, "node": f"0x{box.node:04X}", "schema": msg.SCHEMA_CONTROL,
                "payload": msg.control(command).hex(), "ack": True, "ttl_s": 300,
                "ref": f"control:{box.id}:{command}"}
        return (self.cfg.down_topic, _json(body))

    # -------------------------------------------------------------- status

    def down_status(self, raw: bytes) -> None:
        try:
            st = json.loads(raw)
        except ValueError:
            return
        if st.get("status") in ("failed", "refused"):
            self.failed_verdicts += 1
            log.warning("downlink %s %s: %s", st.get("ref"), st.get("status"), st.get("detail"))
        else:
            log.debug("downlink %s %s", st.get("ref"), st.get("status"))

    # ---------------------------------------------------------------- tick

    def tick(self) -> None:
        """Once a second: follow the operator, and end the run at its deadline."""
        self.refresh()
        run = self.run
        if run is None or run.state != "running" or run.deadline is None:
            return
        if self.clock() >= run.deadline:
            try:
                with self.store.change(run) as append:
                    if run.state == "running":
                        append({"kind": "run_ended", "at": run.deadline, "reason": "deadline"})
                        log.info("run %s ended at its deadline", run.id)
            except GameError:
                pass


def _json(obj) -> bytes:
    return json.dumps(obj, separators=(",", ":")).encode()


def connect(cfg: ServiceConfig, client_id: str, on_message=None, topics=()):
    import paho.mqtt.client as mqtt

    def on_connect(client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            log.error("broker refused the connection: %s", reason_code)
            return
        log.info("connected to %s:%d", cfg.host, cfg.port)
        if topics:
            client.subscribe([(t, cfg.qos) for t in topics])

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id,
                         clean_session=not topics)
    if cfg.username:
        client.username_pw_set(cfg.username, cfg.password)
    if cfg.tls:
        client.tls_set(ca_certs=cfg.ca_file, tls_version=ssl.PROTOCOL_TLS_CLIENT)
    client.on_connect = on_connect
    if on_message:
        client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=60)
    client.connect_async(cfg.host, cfg.port)
    client.loop_start()
    return client


def run(game: Game, store: Store, cfg: ServiceConfig, stop: threading.Event) -> None:
    service = Service(game, store, cfg)
    lock = threading.Lock()  # paho's thread and the ticker share the service

    def on_message(client, userdata, m):
        try:
            with lock:
                if m.topic == cfg.status_topic:
                    service.down_status(m.payload)
                    return
                out = service.uplink(m.payload)
        except Exception:  # a bug: log it, and keep paho's thread alive
            log.exception("could not handle a message on %s", m.topic)
            return
        for topic, body in out:
            client.publish(topic, body, qos=cfg.qos)

    client = connect(cfg, f"{cfg.name}-service", on_message, [cfg.up_topic, cfg.status_topic])
    try:
        while not stop.wait(1.0):
            with lock:
                service.tick()
    finally:
        client.loop_stop()
        client.disconnect()
