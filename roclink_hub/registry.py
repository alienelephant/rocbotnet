"""Device types and nodes: what each node is, so the hub knows how to read it
and how to reach it.

- **Schemas belong to device types** (PROTOCOL.md §5). A device type is one
  maker's firmware and data model; it owns schemas 1-255, so makers never have
  to coordinate schema numbers. The hub decodes by (device type, schema).
- **Node classes** (PROTOCOL.md §1): a sleepy node only hears downlink in the
  reply window after its own uplink; an always-listening node can be sent
  downlink at any time.

A node that isn't registered is sleepy, and is decoded by the deployment-wide
[schemas] table, as before device types existed.
"""

from dataclasses import dataclass, field

SLEEPY = "sleepy"
ALWAYS_LISTENING = "always_listening"
NODE_CLASSES = (SLEEPY, ALWAYS_LISTENING)


@dataclass(frozen=True)
class DeviceType:
    name: str
    node_class: str = SLEEPY
    schemas: dict[int, str] = field(default_factory=dict)  # schema -> decoder name


@dataclass(frozen=True)
class NodeEntry:
    """Nodes on `net` (inclusive address ranges) that are of type `device_type`."""

    net: int
    ranges: tuple[tuple[int, int], ...]
    device_type: str

    def matches(self, net_id: int, node_id: int) -> bool:
        return net_id == self.net and any(lo <= node_id <= hi for lo, hi in self.ranges)


class Registry:
    def __init__(self, types: dict[str, DeviceType], nodes: list[NodeEntry],
                 schemas: dict[int, str]):
        self.types = types
        self.nodes = nodes
        self.schemas = schemas  # the deployment-wide table, for unregistered nodes

    def device_type(self, net_id: int, node_id: int) -> DeviceType | None:
        for entry in self.nodes:  # the first matching entry wins
            if entry.matches(net_id, node_id):
                return self.types[entry.device_type]
        return None

    def decoder_for(self, net_id: int, node_id: int, schema: int) -> str | None:
        t = self.device_type(net_id, node_id)
        return t.schemas.get(schema) if t is not None else self.schemas.get(schema)

    def node_class(self, net_id: int, node_id: int) -> str:
        t = self.device_type(net_id, node_id)
        return t.node_class if t is not None else SLEEPY
