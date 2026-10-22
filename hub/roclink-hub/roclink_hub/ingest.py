"""The ingest process: drains the gateways' MQTT topics, parses each message,
and queues it for the writer of the database it belongs in. It never touches
a database.

- Uplinks on a public net go to the public writer (public.db), rate-limited
  and best-effort: when that queue is full, the message is dropped.
- Everything else goes to the private writer (hub.db), including messages
  that couldn't be parsed: they came from a gateway, so they're worth keeping.
- A secured uplink goes through the anti-replay window (replay.py, PROTOCOL.md
  §10.5) first. A replay, a recorded frame sent again to a gateway that never
  heard it, isn't stored: it's counted as a "replay" drop. The same frame from
  another gateway still goes to the writer, which records the reception. The
  window lives in ingest's own small state file, not in a database.
- Rejection reports from gateways (.../rejected), and the hub's own drops,
  are counted by the monitor (monitor.py). Its summaries go to the private
  writer and to roclink/ops/rejected; its alerts go out at once.

Backpressure: when the private writer falls behind and its queue is full,
put() blocks paho's network thread. The broker then holds new messages for
this session until the hub catches up.

Delivery: paho acknowledges a QoS 1 message once on_message returns, which is
when the message is on the queue, before it is committed. Messages still on
the queue are lost if the hub is killed (a clean stop drains it first).
Duplicates are harmless: the writer recognises a redelivered uplink.
"""

import logging
import queue as queue_module

from . import broker
from .monitor import LEVELS, RejectionMonitor
from .process import child_init, now_ms
from .ratelimit import PublicLimiter
from .replay import REPLAY, ReplayGuard
from .record import (
    SAMPLE_MAX,
    Message,
    RecordError,
    RejectionEvent,
    gateway_from_topic,
    is_rejection_topic,
    parse_message,
    parse_rejection,
)

log = logging.getLogger(__name__)


def run(cfg, private_queue, public_queue, stop) -> None:
    parent_gone = child_init(cfg.log_level)
    public_nets = set(cfg.public.nets)
    limiter = PublicLimiter(cfg.public.max_per_minute, cfg.public.max_per_node_per_minute)
    mon = cfg.monitor
    monitor = RejectionMonitor(mon.window_s, mon.burst, mon.max_keys)
    replay = ReplayGuard(cfg.state_path("ingest-replay.db"))

    def note(client, event: RejectionEvent, at: int) -> None:
        alert = monitor.record(event, at)
        if alert is not None:
            log.warning(
                "ALERT %s: %s from gateway %s, net %s, claimed src %s",
                alert.severity,
                alert.reason,
                _hex(alert.gateway_id),
                _hex(alert.net_id),
                _hex(alert.claimed_src),
            )
            client.publish(mon.topic, alert.to_json(), qos=1)

    def on_message(client, userdata, msg):
        received_at = now_ms()
        if is_rejection_topic(msg.topic):
            try:
                event = parse_rejection(msg.topic, msg.payload)
            except RecordError as e:
                event = RejectionEvent(
                    "malformed_report",
                    "hub",
                    gateway_from_topic(msg.topic),
                    sample=msg.payload[:SAMPLE_MAX],
                    detail=str(e),
                )
            note(client, event, received_at)
            return

        try:
            message = parse_message(msg.topic, msg.payload, received_at, cfg.registry)
        except Exception as e:  # a parser bug: record it, and keep paho's thread alive
            log.exception("could not parse message on %s", msg.topic)
            message = Message(received_at, msg.topic, msg.payload, None, f"parse failed: {e}")

        up = message.uplink
        if up is None:
            event = RejectionEvent(
                "malformed_record",
                "hub",
                gateway_from_topic(msg.topic),
                sample=msg.payload[:SAMPLE_MAX],
                detail=message.error,
            )
            note(client, event, received_at)
            private_queue.put(message)
        elif up.net_id not in public_nets:
            if up.fcnt is not None:
                if replay.check(up.net_id, up.node_id, up.key_gen, up.fcnt) == REPLAY:
                    event = RejectionEvent(
                        "replay",
                        "hub",
                        up.gateway_id,
                        up.net_id,
                        up.node_id,
                        sample=msg.payload[:SAMPLE_MAX],
                        rssi=up.rssi,
                        detail="below the node's anti-replay window",
                    )
                    note(client, event, received_at)
                    return
                monitor.note_verified(up.net_id, up.node_id)
            private_queue.put(message)
        else:
            if not limiter.allow(up.net_id, up.node_id):
                reason = "rate_limited"
            else:
                try:
                    public_queue.put_nowait(message)
                    return
                except queue_module.Full:
                    reason = "queue_full"
            event = RejectionEvent(reason, "hub", up.gateway_id, up.net_id, up.node_id)
            note(client, event, received_at)

    client = broker.connect(
        cfg.mqtt, cfg.mqtt.client_id, cfg.mqtt.topics + cfg.mqtt.rejected_topics, on_message
    )

    def flush(final: bool) -> None:
        summaries = monitor.flush(now_ms(), final)
        for s in summaries:
            client.publish(mon.topic, s.to_json(), qos=1)
            private_queue.put(s)
        if summaries:
            worst = max(summaries, key=lambda s: LEVELS.index(s.severity))
            log.info(
                "rejections: %d summaries, %d events, worst %s (%s)",
                len(summaries),
                sum(s.count for s in summaries),
                worst.severity,
                worst.reason,
            )

    broker.wait_then_disconnect(client, stop, parent_gone, tick=flush)
    log.info("stopped")


def _hex(value: int | None) -> str:
    return "?" if value is None else f"0x{value:X}"
