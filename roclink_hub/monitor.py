"""Counting rejected frames, to spot attacks (GATEWAY_INTERFACE.md §4).

Ingest feeds every RejectionEvent into one RejectionMonitor:
- gateways' reports, from roclink/in/<net>/<gateway>/rejected;
- the hub's own drops: records on roclink/in that don't parse, reports that
  don't parse, and public uplinks over the rate limit or the queue size.

The monitor counts them per window (window_s) by source, gateway, net,
claimed src and reason. When a window closes, ingest publishes each summary
to roclink/ops/rejected and stores it in hub.db's `rejections` table. The
moment a summary first reaches "high" or "critical" (a conflict, a revoked
node, a burst of failures), it's also published as an immediate alert.

Why count instead of passing each event on: a forged frame costs an attacker
one transmission. One broker message and one database row per frame would
let a flood spread into the broker and onto the SD card. Summaries are
bounded instead:
- claimed src is kept only for nodes the hub has seen send verified
  uplinks, and for conflict and revoked (the only reasons a claim can't be
  invented: a conflict's header is verified, and only revoked addresses are
  ever reported as revoked). Every other claim is counted as "unknown";
- at most max_keys summaries per window; past that, by reason only.
"""

import json
import threading
from dataclasses import dataclass, replace

from .record import RejectionEvent

LEVELS = ("info", "warn", "high", "critical")

SEVERITY = {
    # Verified: a second device holds the node's key, or its counter was reset.
    "conflict": "critical",
    # Checked before any cryptography, so anyone can claim a revoked address.
    # Still worth an alert: it may be the stolen device.
    "revoked": "high",
    # Hub: only gateways can publish on roclink/in, so this is a gateway bug
    # or a stolen gateway credential.
    "malformed_record": "high",
    "malformed_report": "high",
    "auth": "warn",
    "replay": "warn",
    "policy": "warn",
    "peers_full": "warn",
    "key": "warn",
    "not_enrolled": "warn",
    "epoch": "warn",
    "queue_full": "warn",  # hub: the public writer is behind
    # Expected on shared bands, or ordinary noise.
    "malformed": "info",
    "net": "info",
    "addr": "info",
    "relayed": "info",
    "unexpected": "info",
    "rate_limited": "info",  # hub: public uplinks over the limit
}
UNKNOWN_REASON_SEVERITY = "warn"
# Many of these from one source in a window is an attack, not an accident.
BURST_REASONS = {"auth", "replay", "policy", "peers_full"}
KEEP_SRC_REASONS = {"conflict", "revoked"}


def severity(reason: str, count: int, burst: int) -> str:
    if reason in BURST_REASONS and count >= burst:
        return "high"
    return SEVERITY.get(reason, UNKNOWN_REASON_SEVERITY)


@dataclass(slots=True)
class RejectionSummary:
    window_start: int  # unix ms
    window_s: int
    source: str  # "gateway" or "hub"
    gateway_id: int | None
    net_id: int | None
    claimed_src: int | None  # None: a node the hub doesn't know, or lumped together
    reason: str
    severity: str
    count: int
    first_at: int
    last_at: int
    rssi_min: float | None = None
    rssi_max: float | None = None
    sample: bytes | None = None  # the first event's
    detail: str | None = None  # the first event's
    immediate: bool = False  # an alert, sent before its window closed
    alerted: bool = False  # internal: an alert for this summary has gone out

    def to_json(self) -> bytes:
        rec = {
            "v": 1,
            "kind": "alert" if self.immediate else "rejections",
            "severity": self.severity,
            "reason": self.reason,
            "source": self.source,
            "gateway": self.gateway_id,
            "net": self.net_id,
            "claimed_src": self.claimed_src,
            "count": self.count,
            "window_start": self.window_start,
            "window_s": self.window_s,
            "first_at": self.first_at,
            "last_at": self.last_at,
            "rssi_min": self.rssi_min,
            "rssi_max": self.rssi_max,
            "sample": self.sample.hex() if self.sample is not None else None,
            "detail": self.detail,
        }
        return json.dumps({k: v for k, v in rec.items() if v is not None}).encode()


class RejectionMonitor:
    """Thread-safe: paho's thread records, ingest's main thread flushes."""

    def __init__(self, window_s: int, burst: int, max_keys: int):
        self.window_s = window_s
        self.window_ms = window_s * 1000
        self.burst = burst
        self.max_keys = max_keys
        self.lock = threading.Lock()
        self.window_start: int | None = None
        self.open: dict[tuple, RejectionSummary] = {}
        self.closed: list[RejectionSummary] = []
        self.known: set[tuple[int, int]] = set()  # (net, node) seen in verified uplinks

    def note_verified(self, net_id: int, node_id: int) -> None:
        with self.lock:
            self.known.add((net_id, node_id))

    def record(self, ev: RejectionEvent, at: int) -> RejectionSummary | None:
        """Count one event. Returns an alert to publish now, if this event
        made its summary "high" or "critical" for the first time."""
        with self.lock:
            self._roll(at)
            key = self._key(ev)
            s = self.open.get(key)
            if s is None:
                s = self.open[key] = RejectionSummary(
                    window_start=self.window_start,
                    window_s=self.window_s,
                    source=key[0],
                    gateway_id=key[1],
                    net_id=key[2],
                    claimed_src=key[3],
                    reason=key[4],
                    severity="info",
                    count=0,
                    first_at=at,
                    last_at=at,
                    sample=ev.sample,
                    detail=ev.detail,
                )
            s.count += ev.count
            s.last_at = at
            if ev.rssi is not None:
                s.rssi_min = ev.rssi if s.rssi_min is None else min(s.rssi_min, ev.rssi)
                s.rssi_max = ev.rssi if s.rssi_max is None else max(s.rssi_max, ev.rssi)
            s.severity = severity(s.reason, s.count, self.burst)
            if LEVELS.index(s.severity) >= LEVELS.index("high") and not s.alerted:
                s.alerted = True
                return replace(s, immediate=True)
            return None

    def flush(self, at: int, final: bool = False) -> list[RejectionSummary]:
        """Summaries of the windows that have closed by `at`. With final, the
        open window's too (on shutdown)."""
        with self.lock:
            self._roll(at)
            out, self.closed = self.closed, []
            if final:
                out.extend(self.open.values())
                self.open = {}
            return out

    def _roll(self, at: int) -> None:
        start = at - at % self.window_ms
        if self.window_start is None:
            self.window_start = start
        elif start != self.window_start:
            self.closed.extend(self.open.values())
            self.open = {}
            self.window_start = start

    def _key(self, ev: RejectionEvent) -> tuple:
        src = ev.claimed_src
        if ev.reason not in KEEP_SRC_REASONS and (ev.net_id, src) not in self.known:
            src = None
        key = (ev.source, ev.gateway_id, ev.net_id, src, ev.reason)
        if key not in self.open and len(self.open) >= self.max_keys:
            # Full: count by reason alone. Unknown reasons (any string a
            # gateway sends) share one key, so this stays bounded too.
            reason = ev.reason if ev.reason in SEVERITY else "other"
            key = (ev.source, None, None, None, reason)
        return key
