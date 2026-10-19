"""The router process: delivers uplinks to services and to the public topic.

It reads the same gateway topics as ingest, with its own MQTT session, so a
service that falls behind can't hold up storage, and a slow writer can't
hold up services. The broker keeps each session's backlog separately.

- An uplink on a private net goes to every service whose [[route]] matches
  its net, node and schema, at roclink/svc/<service>/up. Uplinks with security
  "none" are delivered only to routes with allow_insecure = true.
- An uplink on a public net goes to roclink/pub/up, rate-limited. It never
  reaches a service: config.py refuses routes on a public net.
- Messages that don't parse go nowhere; ingest records them.
- A secured uplink goes through the anti-replay window first (replay.py,
  PROTOCOL.md §10.5): the same frame from another gateway is delivered once,
  and a recorded frame replayed to a gateway that never heard it isn't
  delivered at all. The window is persisted in the router's own state file.
- An unsecured uplink heard by several gateways is delivered once if the
  copies arrive within the dedupe window, which is in memory only. A service
  should still ignore repeats of (net, src, key_gen, fcnt), or (net, src, seq)
  when unsecured: a QoS 1 redelivery can repeat one.
- Routes can match device types (registry.py) as well as nets, nodes and schemas.

Each delivery is the forward record (record.to_record) as JSON. Who may read
which service topic is up to the broker's ACL (see README).
"""

import json
import logging
import time
from collections import Counter, OrderedDict

from . import broker
from .process import child_init, now_ms
from .ratelimit import PublicLimiter
from .record import parse_message, to_record
from .replay import REPLAY, ReplayGuard

log = logging.getLogger(__name__)


def run(cfg, stop) -> None:
    parent_gone = child_init(cfg.log_level)
    router = Router(cfg, replay=ReplayGuard(cfg.state_path("router-replay.db")))

    def on_message(client, userdata, msg):
        try:
            deliveries = router.route(msg.topic, msg.payload, now_ms())
        except Exception:  # a bug: log it, and keep paho's thread alive
            log.exception("could not route message on %s", msg.topic)
            return
        for topic, body in deliveries:
            client.publish(topic, body, qos=cfg.mqtt.qos)
        router.report_if_due()

    # Uplinks only: rejection reports are ingest's.
    client = broker.connect(cfg.mqtt, f"{cfg.mqtt.client_id}-router", cfg.mqtt.topics, on_message)
    log.info(
        "routing to %s%s",
        ", ".join(sorted({r.service for r in cfg.routes})) or "no services",
        " and the public topic" if cfg.public.enabled else "",
    )
    broker.wait_then_disconnect(client, stop, parent_gone)
    log.info("stopped")


class Router:
    """Decides where each message goes. No MQTT here, so it's easy to test."""

    def __init__(self, cfg, clock=time.monotonic, replay: ReplayGuard | None = None):
        self.cfg = cfg
        self.replay = replay if replay is not None else ReplayGuard(":memory:")
        self.public_nets = set(cfg.public.nets)
        self.limiter = PublicLimiter(
            cfg.public.max_per_minute, cfg.public.max_per_node_per_minute, clock
        )
        self.clock = clock
        self.recent = RecentKeys(cfg.writer.dedupe_window_s, clock)
        self.stats: Counter = Counter()
        self.next_report = clock() + cfg.writer.stats_interval_s

    def route(self, topic: str, raw: bytes, received_at: int) -> list[tuple[str, bytes]]:
        """The (topic, body) pairs to publish for one gateway message."""
        message = parse_message(topic, raw, received_at, self.cfg.registry)
        up = message.uplink
        if up is None:
            return []

        if up.net_id in self.public_nets:
            # Rate limit first: forged public traffic mustn't fill the dedupe keys.
            if not self.limiter.allow(up.net_id, up.node_id):
                self.stats["public dropped"] += 1
                return []
            if self._duplicate(up):
                return []
            self.stats["public"] += 1
            return [(self.cfg.router.public_topic, _body(up, received_at))]

        if up.fcnt is not None:  # secured: the anti-replay window decides
            verdict = self.replay.check(up.net_id, up.node_id, up.key_gen, up.fcnt)
            if verdict != "new":
                self.stats["replay" if verdict == REPLAY else "duplicate"] += 1
                return []
        elif self._duplicate(up):
            return []
        targets = []
        for route in self.cfg.routes:
            if route.matches(up.net_id, up.node_id, up.schema, up.device_type):
                if up.security == "none" and not route.allow_insecure:
                    self.stats[f"{route.service} refused insecure"] += 1
                    continue
                targets.append(route.service)
        if not targets:
            self.stats["unrouted"] += 1
            return []
        body = _body(up, received_at)
        out = []
        for service in dict.fromkeys(targets):  # once per service, even if two routes match
            self.stats[service] += 1
            out.append((self.cfg.router.service_topic.format(service=service), body))
        return out

    def _duplicate(self, up) -> bool:
        if self.recent.seen(_uplink_key(up)):
            self.stats["duplicate"] += 1
            return True
        return False

    def report_if_due(self) -> None:
        if self.clock() < self.next_report:
            return
        if self.stats:
            summary = ", ".join(f"{n} {what}" for what, n in sorted(self.stats.items()))
            log.info("last %ds: %s", self.cfg.writer.stats_interval_s, summary)
        self.stats.clear()
        self.next_report = self.clock() + self.cfg.writer.stats_interval_s


class RecentKeys:
    """Keys seen within the last `window_s` seconds."""

    def __init__(self, window_s: float, clock):
        self.window = window_s
        self.clock = clock
        self.keys: OrderedDict = OrderedDict()  # key -> time first seen, oldest first

    def seen(self, key) -> bool:
        now = self.clock()
        while self.keys:
            oldest, at = next(iter(self.keys.items()))
            if now - at <= self.window:
                break
            del self.keys[oldest]
        if key in self.keys:
            return True
        self.keys[key] = now
        return False


def _uplink_key(up) -> tuple:
    """What makes two receptions the same uplink (as in writer.find_duplicate)."""
    if up.fcnt is not None:
        return (up.net_id, up.node_id, up.key_gen, up.fcnt)
    return (up.net_id, up.node_id, up.seq, up.schema, up.payload)


def _body(up, received_at: int) -> bytes:
    return json.dumps(to_record(up, received_at), separators=(",", ":")).encode()
