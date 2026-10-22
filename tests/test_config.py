import pytest

from roclink_hub import config

BASE = '[database]\npath = "hub.db"\n'


def load(tmp_path, text: str):
    p = tmp_path / "c.toml"
    p.write_text(BASE + text)
    return config.load(str(p))


def test_the_example_config_loads():
    cfg = config.load("config.example.toml")
    assert [r.service for r in cfg.routes] == ["scavhunt"]
    assert cfg.public.nets == [0xF0] and cfg.public.api.enabled


def test_defaults_have_no_routes_and_no_public_net(tmp_path):
    cfg = load(tmp_path, "")
    assert cfg.routes == [] and not cfg.public.enabled and not cfg.public.api.enabled
    assert cfg.mqtt.topics == ["roclink/in/+/+/up"]


def test_route_fields(tmp_path):
    cfg = load(
        tmp_path,
        '[[route]]\nservice = "scavhunt"\nnet = 0x42\nschemas = [20]\n'
        'nodes = ["0x0100-0x01FF", 0x1234]\nallow_insecure = true\n',
    )
    (r,) = cfg.routes
    assert (r.service, r.net, r.schemas, r.allow_insecure) == ("scavhunt", 0x42, {20}, True)
    assert r.nodes == ((0x100, 0x1FF), (0x1234, 0x1234))
    assert r.matches(0x42, 0x150, 20)
    assert not r.matches(0x42, 0x150, 21)
    assert not r.matches(0x42, 0x200, 20)
    assert not r.matches(0x43, 0x150, 20)


def test_device_types_nodes_and_downlink(tmp_path):
    cfg = load(
        tmp_path,
        '[[device_type]]\nname = "valve"\nclass = "always_listening"\nschemas = { 10 = "event" }\n'
        '[[node]]\nnet = 0x42\nnodes = ["0x0200-0x02FF"]\ntype = "valve"\n'
        '[[route]]\nservice = "valves"\nnet = 0x42\ntypes = ["valve"]\n'
        '[downlink]\nmax_attempts = 3\n',
    )
    reg = cfg.registry
    assert reg.node_class(0x42, 0x0210) == "always_listening"
    assert reg.node_class(0x42, 0x0310) == "sleepy"
    assert reg.decoder_for(0x42, 0x0210, 10) == "event"
    assert cfg.routes[0].matches(0x42, 0x0210, 1, "valve")
    assert not cfg.routes[0].matches(0x42, 0x0210, 1, None)
    assert cfg.downlink.max_attempts == 3
    assert cfg.state_path("x.db").endswith("/x.db")


@pytest.mark.parametrize(
    "text, error",
    [
        ('[[route]]\nservice = "Scav Hunt"\nnet = 1\n', "service must match"),
        ('[[route]]\nservice = "a/b"\nnet = 1\n', "service must match"),
        ('[[route]]\nservice = "roclink-hub"\nnet = 1\n', "reserved"),
        ('[[route]]\nservice = "x"\n', "net is required"),
        ('[[route]]\nservice = "x"\nnet = 300\n', "not a net id"),
        ('[[route]]\nservice = "x"\nnet = 1\nschemas = [0]\n', "schemas"),
        ('[[route]]\nservice = "x"\nnet = 1\nnodes = ["0x10-0x01"]\n', "node range"),
        ('[[route]]\nservice = "x"\nnet = 1\nnodes = ["nope"]\n', "bad node"),
        ('[[route]]\nservice = "x"\nnet = 1\ncolour = "red"\n', "unknown settings"),
        (
            '[public]\nnets = [0xF0]\ndb_path = "p.db"\n[[route]]\nservice = "x"\nnet = 0xF0\n',
            "public",
        ),
        ("[public]\nnets = [0xF0]\n", "db_path is required"),
        ('[public]\nnets = [0xF0]\ndb_path = "hub.db"\n', "must differ"),
        ('[public]\nnets = [1]\ndb_path = "p.db"\n[public.api]\nenabled = true\nport = 8080\n', "port must differ"),
        ('[[device_type]]\nname = "v"\nclass = "chatty"\n', "class must be"),
        ('[[device_type]]\nname = "v"\n[[device_type]]\nname = "v"\n', "defined twice"),
        ('[[device_type]]\nname = "v"\nschemas = { 1 = "nope" }\n', "unknown decoder"),
        ('[[node]]\nnet = 1\nnodes = [1]\ntype = "v"\n', "type must be"),
        ('[[route]]\nservice = "x"\nnet = 1\ntypes = ["v"]\n', "unknown device types"),
        ('[downlink]\nspeed = 1\n', "unknown settings"),
    ],
)
def test_rejects(tmp_path, text, error):
    with pytest.raises(config.ConfigError, match=error):
        load(tmp_path, text)


LAYOUT = """
[decoder.vault-reading]
struct = "<hfB"
fields = ["temp_cC", "humidity", "flags"]
scale = { temp_cC = 0.01 }
bits = { flags = ["door_open", "", "lid_closed"] }

[[device_type]]
name = "scavhunt-box"
class = "always_listening"
schemas = { 32 = "vault-reading" }

[[node]]
net = 0x42
nodes = ["0x0201"]
type = "scavhunt-box"
"""


def test_a_decoder_declared_in_the_config(tmp_path):
    import struct

    from roclink_hub.record import parse_message

    cfg = load(tmp_path, LAYOUT)
    payload = struct.pack("<hfB", 2215, 48.5, 0b101)
    frame = bytes([0x20, 0x00, 0x42]) + struct.pack("<HHBB", 0x0201, 0, 1, 32) + payload
    up = parse_message("roclink/in/42/ff01/up", frame, 1000, cfg.registry).uplink
    assert (up.device_type, up.decoder) == ("scavhunt-box", "vault-reading")
    assert up.decoded == {"temp_cC": pytest.approx(22.15), "humidity": 48.5, "door_open": 1, "lid_closed": 1}
    short = parse_message("roclink/in/42/ff01/up", frame[:-1], 1000, cfg.registry).uplink
    assert short.decoded is None and "vault-reading payload is 7 bytes" in short.decode_error


@pytest.mark.parametrize("table, error", [
    ('struct = "hB"\nfields = ["a", "b"]', 'starting with "<" or ">"'),
    ('struct = "<hB"\nfields = ["a"]', "has 2 fields"),
    ('struct = "<hB"\nfields = ["a", "a"]', "has 2 fields"),
    ('struct = "<hQq!"\nfields = ["a", "b", "c"]', "struct"),
    ('struct = "<hB"\nfields = ["a", "b"]\nscale = { c = 2 }', "scale must be a table of field names"),
    ('struct = "<hB"\nfields = ["a", "b"]\ncolour = 1', "unknown settings colour"),
])
def test_rejects_a_bad_declared_decoder(tmp_path, table, error):
    with pytest.raises(config.ConfigError, match=error):
        load(tmp_path, f"[decoder.bad]\n{table}\n")


def test_rejects_an_undeclared_decoder_name(tmp_path):
    with pytest.raises(config.ConfigError, match="unknown decoder 'nope'"):
        load(tmp_path, '[schemas]\n32 = "nope"\n')
