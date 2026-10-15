import json
import threading

import pytest

from scavhunt import api
from scavhunt import messages as m
from scavhunt.__main__ import main
from scavhunt.game import GameError, Verdict, parse_game
from scavhunt.service import Service, ServiceConfig
from scavhunt.store import Store

from .test_game import BLUE, GAME, RED, T0


@pytest.fixture
def env(tmp_path):
    game = parse_game(GAME)
    store = Store(str(tmp_path / "game.db"))
    run = store.create_run(game, "r1", 0)
    with store.change(run) as append:
        append({"kind": "team_entered", "at": 0, "team": "red", "name": "Red", "tokens": [RED.hex()]})
        append({"kind": "team_entered", "at": 0, "team": "blue", "tokens": [BLUE.hex()]})
        append({"kind": "run_started", "at": T0})
    clock = [T0]
    svc = Service(game, store, ServiceConfig(), clock=lambda: clock[0])
    return game, store, svc, clock


_fcnt = iter(range(1, 10_000))


def record(node, token, at, schema=m.SCHEMA_WIN, sec="enc", fcnt=None, **extra):
    rec = {"v": 1, "net": 0x42, "src": node, "dst": 1, "seq": 7, "schema": schema, "flags": 1,
           "sec": sec, "payload": m.token_message(token).hex(), "hub_time": at,
           "fcnt": next(_fcnt) if fcnt is None else fcnt, "key_gen": 0, "gateway": 1,
           "device_type": "scavhunt-box", **extra}
    if sec == "none":
        del rec["fcnt"], rec["key_gen"]
    return json.dumps(rec).encode()


def verdict_of(out):
    assert len(out) == 1
    topic, body = out[0]
    assert topic == "roclink/svc/scavhunt/down"
    req = json.loads(body)
    assert req["schema"] == m.SCHEMA_VERDICT and req["ack"] is True
    p = bytes.fromhex(req["payload"])
    return req, (p[1], p[2], p[3], int.from_bytes(p[4:6], "little"))


def test_win_gets_a_verdict(env):
    game, store, svc, _ = env
    req, v = verdict_of(svc.uplink(record(0x201, RED, T0 + 1)))
    assert req["node"] == "0x0201" and req["net"] == 0x42 and req["ttl_s"] == 60
    assert v == (Verdict.ACCEPTED, 1, 4, 0)
    assert store.load(game, "r1").teams["red"].solved == {"vault": T0 + 1}


def test_locked_verdict_names_a_node(env):
    _, _, svc, _ = env
    _, v = verdict_of(svc.uplink(record(0x204, RED, T0 + 1)))
    assert v == (Verdict.LOCKED, 0, 4, 0x201)


def test_repeated_delivery_is_ignored(env):
    game, store, svc, _ = env
    raw = record(0x201, RED, T0 + 1, fcnt=500)
    assert svc.uplink(raw)
    assert svc.uplink(raw) == []
    assert len([e for e in store.events("r1") if e["kind"] == "claim"]) == 1


def test_insecure_refused_unless_allowed(env, tmp_path):
    game, store, svc, _ = env
    assert svc.uplink(record(0x201, RED, T0 + 1, sec="auth")) == []
    assert svc.uplink(record(0x201, RED, T0 + 1, sec="none")) == []
    bench = Service(game, store, ServiceConfig(allow_insecure=True))
    _, v = verdict_of(bench.uplink(record(0x201, RED, T0 + 1, sec="none")))
    assert v[0] == Verdict.ACCEPTED


def test_strays_are_ignored(env):
    _, store, svc, _ = env
    assert svc.uplink(record(0x2FF, RED, T0 + 1)) == []  # not a box
    assert svc.uplink(record(0x201, RED, T0 + 1, schema=40)) == []  # not a game schema
    assert svc.uplink(b"{not json") == []
    bad = json.loads(record(0x201, RED, T0 + 1))
    bad["payload"] = "0205"
    assert svc.uplink(json.dumps(bad).encode()) == []
    assert [e["kind"] for e in store.events("r1")] == ["team_entered", "team_entered", "run_started"]


def test_attempt_is_recorded_without_reply(env):
    _, store, svc, _ = env
    assert svc.uplink(record(0x203, BLUE, T0 + 1, schema=m.SCHEMA_ATTEMPT)) == []
    ev = store.events("r1")[-1]
    assert ev["kind"] == "attempt" and ev["team"] == "blue" and ev["box"] == "garden"


def test_unknown_team_is_recorded(env):
    _, store, svc, _ = env
    _, v = verdict_of(svc.uplink(record(0x201, b"\x77", T0 + 1)))
    assert v[0] == Verdict.UNKNOWN_TEAM
    assert store.events("r1")[-1]["verdict"] == Verdict.UNKNOWN_TEAM


def test_finish_and_restart_rebuilds_state(env, tmp_path):
    game, store, svc, _ = env
    for i, node in enumerate([0x201, 0x203, 0x202]):
        svc.uplink(record(node, RED, T0 + i))
    _, v = verdict_of(svc.uplink(record(0x204, RED, T0 + 10)))
    assert v[:3] == (Verdict.FINISHED, 4, 4)
    again = Service(game, Store(str(tmp_path / "game.db")), ServiceConfig())
    assert again.run.teams["red"].finished_at == T0 + 10
    assert again.run.standings()[0]["team"] == "red"


def test_deadline_ends_the_run(env):
    game, store, svc, clock = env
    clock[0] = T0 + 3600_000
    svc.tick()
    assert svc.run.state == "ended" and svc.run.ended_at == T0 + 3600_000
    svc.tick()  # only once
    assert [e["kind"] for e in store.events("r1")].count("run_ended") == 1
    _, v = verdict_of(svc.uplink(record(0x201, RED, T0 + 3600_001)))
    assert v[0] == Verdict.NOT_RUNNING


def test_service_follows_operator_changes(env, tmp_path):
    game, _, svc, _ = env
    other = Store(str(tmp_path / "game.db"))  # the operator's CLI, another process
    run = other.load(game, "r1")
    with other.change(run) as append:
        append({"kind": "correction", "at": T0 + 1, "team": "red", "box": "vault", "action": "add"})
    _, v = verdict_of(svc.uplink(record(0x202, RED, T0 + 2)))  # lab needs vault
    assert v[0] == Verdict.ACCEPTED
    other.create_run(game, "r2", 1)  # a new run takes over
    svc.tick()
    assert svc.run.id == "r2" and svc.run.state == "open"


def test_refused_change_leaves_nothing(env):
    game, store, svc, _ = env
    run = store.load(game, "r1")
    with pytest.raises(GameError):
        with store.change(run) as append:
            append({"kind": "team_entered", "at": 1, "team": "red", "tokens": ["01"]})
    assert len(store.events("r1")) == 3
    assert set(run.teams) == {"red", "blue"}


def test_control_request(env):
    _, _, svc, _ = env
    topic, body = svc.control("tower", m.CONTROL_RESET)
    req = json.loads(body)
    assert topic == "roclink/svc/scavhunt/down"
    assert (req["node"], req["schema"], req["payload"]) == ("0x0204", 11, "0101")
    with pytest.raises(GameError):
        svc.control("nowhere", 1)


def test_no_run_yet(tmp_path):
    game = parse_game(GAME)
    svc = Service(game, Store(str(tmp_path / "x.db")), ServiceConfig())
    _, v = verdict_of(svc.uplink(record(0x201, RED, T0)))
    assert v[0] == Verdict.NOT_RUNNING


def test_api_views(env):
    game, store, svc, _ = env
    svc.uplink(record(0x201, RED, T0 + 1))
    view = api.run_view(game, store)
    red = next(t for t in view["run"]["teams"] if t["team"] == "red")
    assert red["name"] == "Red" and red["solved"] == {"vault": T0 + 1}
    assert red["open"] == ["lab", "garden", "cellar"]
    assert api.standings(game, store)[0]["team"] == "red"


def test_api_over_http(env, tmp_path):
    import urllib.request
    from http.server import ThreadingHTTPServer

    game, store, svc, _ = env
    svc.uplink(record(0x201, RED, T0 + 1))
    import scavhunt.api as a

    servers = []
    orig = ThreadingHTTPServer.__init__

    def capture(self, *args, **kw):
        orig(self, *args, **kw)
        servers.append(self)

    ThreadingHTTPServer.__init__ = capture
    try:
        t = threading.Thread(target=a.serve, args=(game, str(tmp_path / "game.db"), "127.0.0.1", 0),
                             daemon=True)
        t.start()
        while not servers:
            pass
    finally:
        ThreadingHTTPServer.__init__ = orig
    port = servers[0].server_address[1]
    try:
        get = lambda p: urllib.request.urlopen(f"http://127.0.0.1:{port}{p}").read()
        assert json.loads(get("/api/standings"))[0]["team"] == "red"
        assert json.loads(get("/api/events?after=3"))[0]["kind"] == "claim"
        assert b"<title>Scoreboard</title>" in get("/")
    finally:
        servers[0].shutdown()


def test_cli(tmp_path, capsys):
    gfile = tmp_path / "game.toml"
    gfile.write_text(open("game.example.toml").read())
    db = str(tmp_path / "cli.db")
    run = lambda *args: main(["-g", str(gfile), "--db", db, *args])
    assert run("check") == 0
    assert run("status") == 1  # no run yet
    assert run("new-run", "evening") == 0
    assert run("team", "red", "--name", "Red Team", "--token", "04:a1:b2", "--token", "text:RED7") == 0
    assert run("team", "blue", "--token", "text:RED7") == 1  # token taken
    assert run("start") == 0
    assert run("start") == 1
    assert run("solve", "red", "vault", "--note", "box was offline") == 0
    assert run("solve", "red", "nowhere") == 1
    assert run("status") == 0
    assert "Red Team" in capsys.readouterr().out
    assert run("end") == 0
    game = parse_game(__import__("tomllib").loads(gfile.read_text()))
    r = Store(db).load(game, "evening")
    assert r.state == "ended" and r.tokens[b"RED7"] == "red" and "vault" in r.teams["red"].solved
