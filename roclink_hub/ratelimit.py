"""Rate limits for the public net.

Anyone can send on the public net and claim any node id, so a per-node limit
alone doesn't help: one total limit caps the flood, and per-node limits keep
one noisy device from using up the total.
"""

import time

MAX_TRACKED_NODES = 10_000


class Bucket:
    """A token bucket: `rate_per_min` tokens a minute, holding at most one minute's worth."""

    def __init__(self, rate_per_min: float, now: float):
        self.rate = rate_per_min / 60
        self.capacity = float(rate_per_min)
        self.tokens = self.capacity
        self.updated = now

    def refill(self, now: float) -> None:
        self.tokens = min(self.capacity, self.tokens + (now - self.updated) * self.rate)
        self.updated = now


class PublicLimiter:
    def __init__(self, total_per_min: int, per_node_per_min: int, clock=time.monotonic):
        self.clock = clock
        self.per_node = per_node_per_min
        self.total = Bucket(total_per_min, clock())
        self.nodes: dict[tuple[int, int], Bucket] = {}
        self.dropped = 0

    def allow(self, net_id: int, node_id: int) -> bool:
        now = self.clock()
        node = self.nodes.get((net_id, node_id))
        if node is None:
            if len(self.nodes) >= MAX_TRACKED_NODES:
                self._forget_idle(now)
            node = self.nodes[(net_id, node_id)] = Bucket(self.per_node, now)
        node.refill(now)
        self.total.refill(now)
        if node.tokens < 1 or self.total.tokens < 1:
            self.dropped += 1
            return False
        node.tokens -= 1
        self.total.tokens -= 1
        return True

    def _forget_idle(self, now: float) -> None:
        """Drop nodes whose buckets have refilled: forgetting them changes nothing.
        If a flood of forged ids leaves none idle, start over rather than grow."""
        for key, bucket in list(self.nodes.items()):
            bucket.refill(now)
            if bucket.tokens >= bucket.capacity:
                del self.nodes[key]
        if len(self.nodes) >= MAX_TRACKED_NODES:
            self.nodes.clear()
