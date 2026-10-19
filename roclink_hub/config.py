"""Configuration, loaded from a TOML file. See config.example.toml."""

import os
import re
import tomllib
from dataclasses import dataclass, field, fields
from functools import cached_property

from .decoders import DECODERS
from .registry import NODE_CLASSES, DeviceType, NodeEntry, Registry


class ConfigError(ValueError):
    pass


@dataclass(frozen=True)
class MqttConfig:
    host: str = "localhost"
    port: int = 1883
    # A fixed client id plus clean_session=False makes the broker keep a
    # session: QoS 1 messages published while the hub is down are delivered
    # when it reconnects. The router uses "<client_id>-router".
    client_id: str = "roclink-hub"
    # Gateways publish uplinks to roclink/in/<net>/<gateway>/up, and reports
    # of the frames they dropped to .../rejected (docs/GATEWAY_INTERFACE.md).
    topics: list[str] = field(default_factory=lambda: ["roclink/in/+/+/up"])
    rejected_topics: list[str] = field(default_factory=lambda: ["roclink/in/+/+/rejected"])
    qos: int = 1
    keepalive_s: int = 60
    username: str | None = None
    password: str | None = None
    tls: bool = False
    ca_file: str | None = None  # None with tls = the system's CA store


@dataclass(frozen=True)
class WriterConfig:
    batch_size: int = 200  # most messages per transaction
    flush_interval_s: float = 0.5  # longest a message waits for its batch to fill
    queue_size: int = 10_000  # messages buffered between ingest and writer
    dedupe_window_s: int = 60  # unsecured duplicates (PROTOCOL.md Annex B)
    stats_interval_s: int = 60
    prune_interval_s: int = 60  # how often a writer with a retention period prunes


@dataclass(frozen=True)
class ApiConfig:
    enabled: bool = True
    host: str = "127.0.0.1"
    port: int = 8080


@dataclass(frozen=True)
class RouterConfig:
    enabled: bool = True
    service_topic: str = "roclink/svc/{service}/up"
    public_topic: str = "roclink/pub/up"


@dataclass(frozen=True)
class MonitorConfig:
    """Counting rejected frames: gateways' reports and the hub's own drops."""

    window_s: int = 60  # summaries cover this long
    burst: int = 20  # this many auth/replay/policy/peers_full from one source: "high"
    topic: str = "roclink/ops/rejected"  # summaries and alerts, for operators only
    max_keys: int = 1000  # distinct summaries per window; more are lumped together


@dataclass(frozen=True)
class DownlinkConfig:
    """Downlink: queued at the hub, numbered per node, sent through one gateway
    (PROTOCOL.md §9.1, GATEWAY_INTERFACE.md §5)."""

    enabled: bool = True
    request_topic: str = "roclink/svc/+/down"  # services ask here (their own subtree)
    service_status_topic: str = "roclink/svc/{service}/down_status"
    out_topic: str = "roclink/out/{net:02x}/{gateway:04x}/down"  # hub to one gateway
    status_topic: str = "roclink/in/+/+/down_status"  # gateways answer here
    ttl_s: int = 3600  # how long a downlink may wait for its node, at most
    offer_window_s: int = 600  # a sleepy node's offer: until its next listen window
    offer_now_s: int = 30  # an always-listening node's offer: sent at once
    max_attempts: int = 5  # offers (through any gateway) before giving up


@dataclass(frozen=True)
class Route:
    """Deliver uplinks from `net` (optionally only some schemas, nodes and device
    types) to a service. The same route lets the service send downlink to
    those nodes and schemas."""

    service: str
    net: int
    schemas: frozenset[int] | None = None  # None = every schema
    nodes: tuple[tuple[int, int], ...] | None = None  # inclusive ranges; None = every node
    allow_insecure: bool = False  # deliver uplinks with security "none" too
    types: frozenset[str] | None = None  # device types; None = any (or none)

    def matches(self, net_id: int, node_id: int, schema: int, device_type: str | None = None) -> bool:
        if net_id != self.net:
            return False
        if self.schemas is not None and schema not in self.schemas:
            return False
        if self.types is not None and device_type not in self.types:
            return False
        return self.nodes is None or any(lo <= node_id <= hi for lo, hi in self.nodes)


@dataclass(frozen=True)
class PublicConfig:
    """The public net: unsecured, open to any developer, visible to anyone."""

    nets: list[int] = field(default_factory=list)  # empty = no public net
    db_path: str | None = None
    retention_h: float = 24
    # Anyone can send on the public net, under any node id. These limits cap
    # what reaches public.db and roclink/pub; the rest is dropped.
    max_per_minute: int = 600  # all public nodes together
    max_per_node_per_minute: int = 30
    api: ApiConfig = field(default_factory=lambda: ApiConfig(enabled=False, port=8081))

    @property
    def enabled(self) -> bool:
        return bool(self.nets)


@dataclass(frozen=True)
class Config:
    db_path: str  # private and service uplinks, kept for good
    mqtt: MqttConfig = field(default_factory=MqttConfig)
    writer: WriterConfig = field(default_factory=WriterConfig)
    api: ApiConfig = field(default_factory=ApiConfig)
    router: RouterConfig = field(default_factory=RouterConfig)
    monitor: MonitorConfig = field(default_factory=MonitorConfig)
    downlink: DownlinkConfig = field(default_factory=DownlinkConfig)
    routes: list[Route] = field(default_factory=list)
    public: PublicConfig = field(default_factory=PublicConfig)
    schemas: dict[int, str] = field(default_factory=dict)  # schema -> decoder, for unregistered nodes
    device_types: dict[str, DeviceType] = field(default_factory=dict)
    nodes: list[NodeEntry] = field(default_factory=list)
    state_dir: str | None = None  # small state files; default: beside db_path
    log_level: str = "INFO"

    @cached_property
    def registry(self) -> Registry:
        return Registry(self.device_types, self.nodes, self.schemas)

    def state_path(self, name: str) -> str:
        """Where a process keeps its own small state file (anti-replay windows, the downlink queue)."""
        base = self.state_dir or os.path.dirname(os.path.abspath(self.db_path))
        return os.path.join(base, name)


# Service names are MQTT topic levels and broker usernames (see README).
SERVICE_NAME = re.compile(r"[a-z0-9][a-z0-9_-]{0,63}")
TYPE_NAME = re.compile(r"[a-z0-9][a-z0-9_.-]{0,63}")


def load(path: str) -> Config:
    try:
        with open(path, "rb") as f:
            raw = tomllib.load(f)
    except (OSError, tomllib.TOMLDecodeError) as e:
        raise ConfigError(f"{path}: {e}") from None

    database = raw.pop("database", {})
    if "path" not in database:
        raise ConfigError("[database] path is required")
    unknown_db = set(database) - {"path", "state_dir"}
    if unknown_db:
        raise ConfigError(f"unknown settings in [database]: {', '.join(sorted(unknown_db))}")
    public = _public(raw.pop("public", {}))
    types = _device_types(raw.pop("device_type", []))
    cfg = Config(
        db_path=database["path"],
        state_dir=database.get("state_dir"),
        mqtt=_section(MqttConfig, raw.pop("mqtt", {}), "mqtt"),
        writer=_section(WriterConfig, raw.pop("writer", {}), "writer"),
        api=_section(ApiConfig, raw.pop("api", {}), "api"),
        router=_section(RouterConfig, raw.pop("router", {}), "router"),
        monitor=_section(MonitorConfig, raw.pop("monitor", {}), "monitor"),
        downlink=_section(DownlinkConfig, raw.pop("downlink", {}), "downlink"),
        routes=[_route(r, public, types) for r in raw.pop("route", [])],
        public=public,
        schemas=_ports(raw.pop("schemas", {}), "[schemas]"),
        device_types=types,
        nodes=[_node_entry(n, types) for n in raw.pop("node", [])],
        log_level=raw.pop("log_level", "INFO"),
    )
    if raw:
        raise ConfigError(f"unknown settings: {', '.join(raw)}")
    if public.enabled and public.db_path == cfg.db_path:
        raise ConfigError("[public] db_path must differ from [database] path")
    if public.api.enabled and cfg.api.enabled and public.api.port == cfg.api.port:
        raise ConfigError("[public.api] port must differ from [api] port")
    return cfg


def _section(cls, values: dict, name: str):
    known = {f.name for f in fields(cls)}
    unknown = set(values) - known
    if unknown:
        raise ConfigError(f"unknown settings in [{name}]: {', '.join(sorted(unknown))}")
    return cls(**values)


def _public(values: dict) -> PublicConfig:
    api = _section(ApiConfig, {"enabled": False, "port": 8081, **values.pop("api", {})}, "public.api")
    public = _section(PublicConfig, {**values, "api": api}, "public")
    for net in public.nets:
        _check_net(net, "[public] nets")
    if public.enabled and not public.db_path:
        raise ConfigError("[public] db_path is required when nets are set")
    if public.retention_h <= 0:
        raise ConfigError("[public] retention_h must be positive")
    return public


def _route(values: dict, public: PublicConfig, types: dict[str, DeviceType]) -> Route:
    where = f"[[route]] {values.get('service', '?')}"
    unknown = set(values) - {f.name for f in fields(Route)}
    if unknown:
        raise ConfigError(f"unknown settings in {where}: {', '.join(sorted(unknown))}")
    service = values.get("service")
    if not isinstance(service, str) or not SERVICE_NAME.fullmatch(service):
        raise ConfigError(f"{where}: service must match {SERVICE_NAME.pattern}")
    if service.startswith("roclink"):
        # Broker users roclink-hub and roclink-gw would get this service's data.
        raise ConfigError(f"{where}: service names starting with 'roclink' are reserved")
    if "net" not in values:
        raise ConfigError(f"{where}: net is required")
    net = _check_net(values["net"], f"{where}: net")
    if net in public.nets:
        # Anyone can send on the public net, so a service must never trust it.
        raise ConfigError(f"{where}: net 0x{net:02X} is public and can't be routed to a service")

    schemas = values.get("schemas")
    if schemas is not None:
        if not all(isinstance(p, int) and 1 <= p <= 255 for p in schemas):
            raise ConfigError(f"{where}: schemas must be numbers in 1..255")
        schemas = frozenset(schemas)
    nodes = values.get("nodes")
    if nodes is not None:
        nodes = tuple(_node_range(n, where) for n in nodes)
    allow_insecure = values.get("allow_insecure", False)
    if not isinstance(allow_insecure, bool):
        raise ConfigError(f"{where}: allow_insecure must be true or false")
    route_types = values.get("types")
    if route_types is not None:
        missing = [t for t in route_types if t not in types]
        if missing:
            raise ConfigError(f"{where}: unknown device types {', '.join(map(str, missing))}")
        route_types = frozenset(route_types)
    return Route(service, net, schemas, nodes, allow_insecure, route_types)


def _device_types(entries: list) -> dict[str, DeviceType]:
    """[[device_type]] name = "...", class = "sleepy" | "always_listening", schemas = {1 = "env"}"""
    types = {}
    for values in entries:
        name = values.get("name")
        where = f"[[device_type]] {name or '?'}"
        unknown = set(values) - {"name", "class", "schemas"}
        if unknown:
            raise ConfigError(f"{where}: unknown settings {', '.join(sorted(unknown))}")
        if not isinstance(name, str) or not TYPE_NAME.fullmatch(name):
            raise ConfigError(f"{where}: name must match {TYPE_NAME.pattern}")
        if name in types:
            raise ConfigError(f"{where}: defined twice")
        node_class = values.get("class", "sleepy")
        if node_class not in NODE_CLASSES:
            raise ConfigError(f"{where}: class must be one of {', '.join(NODE_CLASSES)}")
        types[name] = DeviceType(name, node_class, _ports(values.get("schemas", {}), where))
    return types


def _node_entry(values: dict, types: dict[str, DeviceType]) -> NodeEntry:
    """[[node]] net = 0x42, nodes = ["0x1234", "0x0100-0x01FF"], type = "..." """
    where = f"[[node]] {values.get('type', '?')}"
    unknown = set(values) - {"net", "nodes", "type"}
    if unknown:
        raise ConfigError(f"{where}: unknown settings {', '.join(sorted(unknown))}")
    if values.get("type") not in types:
        raise ConfigError(f"{where}: type must be a [[device_type]] name")
    if "net" not in values or not values.get("nodes"):
        raise ConfigError(f"{where}: net and nodes are required")
    net = _check_net(values["net"], f"{where}: net")
    ranges = tuple(_node_range(n, where) for n in values["nodes"])
    return NodeEntry(net, ranges, values["type"])


def _node_range(value, where: str) -> tuple[int, int]:
    """A node id (0x1234 or "0x1234") or an inclusive range ("0x0100-0x01FF")."""
    try:
        if isinstance(value, int) and not isinstance(value, bool):
            lo = hi = value
        else:
            lo_s, _, hi_s = str(value).partition("-")
            lo = int(lo_s.strip(), 0)
            hi = int(hi_s.strip(), 0) if hi_s else lo
    except ValueError:
        raise ConfigError(f"{where}: bad node or range {value!r}") from None
    if not 0x0001 <= lo <= hi <= 0xFEFF:
        raise ConfigError(f"{where}: node range {value!r} is outside 0x0001..0xFEFF")
    return lo, hi


def _check_net(value, where: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 0xFF:
        raise ConfigError(f"{where}: {value!r} is not a net id (0..255)")
    return value


def _ports(values: dict, where: str) -> dict[int, str]:
    if not isinstance(values, dict):
        raise ConfigError(f"{where}: schemas must be a table of schema = decoder")
    schemas = {}
    for key, decoder in values.items():
        try:
            schema = int(key)
        except ValueError:
            raise ConfigError(f"{where}: schema {key!r} is not a number") from None
        if not 1 <= schema <= 255:
            raise ConfigError(f"{where}: schema {schema} is outside 1..255")
        if decoder not in DECODERS:
            raise ConfigError(
                f"{where}: schema {schema}: unknown decoder {decoder!r} (have: {', '.join(DECODERS)})"
            )
        schemas[schema] = decoder
    return schemas
