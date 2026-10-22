import json
import threading

import pytest

from scavhunt import api, service
from scavhunt import messages as m
from scavhunt.__main__ import main
from scavhunt.game import GameError, Verdict, parse_game
from scavhunt.service import Service, ServiceConfig
from scavhunt.store import Store

from .test_game import BLUE, GAME, MIN, RED, S, T0


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


def record(node, token, at, schema=m.SCHEMA_WIN, sec="enc", fcnt=None, hint=None, **extra):
    rec = {"v": 1, "net": 0x42, "src": node, "dst": 1, "seq": 7, "schema": schema, "flags": 1,
           "sec": sec, "payload": m.box_message(token, hint).hex(), "hub_time": at,
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


def play(svc, node, token, at, took=S):
    _, v = verdict_of(svc.uplink(record(node, token, at, schema=m.SCHEMA_START)))
    assert v[0] == Verdict.GO
    return verdict_of(svc.uplink(record(node, token, at + took)))


def test_a_box_is_played(env):
    game, store, svc, _ = env
    req, v = verdict_of(svc.uplink(record(0x201, RED, T0, schema=m.SCHEMA_START)))
    assert req["node"] == "0x0201" and req["net"] == 0x42 and req["ttl_s"] == 60
    assert v == (Verdict.GO, 0, 4, 0)
    _, v = verdict_of(svc.uplink(record(0x201, RED, T0 + S, schema=m.SCHEMA_HINT, hint=1)))
    assert v == (Verdict.HINT, 0, 4, 1)
    assert svc.uplink(record(0x201, RED, T0 + 2 * S, schema=m.SCHEMA_WRONG)) == []
    _, v = verdict_of(svc.uplink(record(0x201, RED, T0 + 10 * S)))
    assert v == (Verdict.ACCEPTED, 1, 4, 0)
    run = store.load(game, "r1")
    red = run.teams["red"]
    assert red.solved == {"vault": T0 + 10 * S}
    assert run.time_ms(red) == 10 * S + 60 * S + 30 * S
    assert [e["kind"] for e in store.events("r1")][3:] == ["start", "hint", "wrong", "claim"]


def test_locked_verdict_names_a_node(env):
    _, _, svc, _ = env
    _, v = verdict_of(svc.uplink(record(0x204, RED, T0 + 1, schema=m.SCHEMA_START)))
    assert v == (Verdict.LOCKED, 0, 4, 0x201)


def test_busy_box(env):
    _, _, svc, _ = env
    verdict_of(svc.uplink(record(0x201, RED, T0, schema=m.SCHEMA_START)))
    _, v = verdict_of(svc.uplink(record(0x201, BLUE, T0 + MIN, schema=m.SCHEMA_START)))
    assert v == (Verdict.BUSY, 0, 4, 9 * 60)
    assert svc.uplink(record(0x201, RED, T0 + 2 * MIN, schema=m.SCHEMA_END)) == []
    _, v = verdict_of(svc.uplink(record(0x201, BLUE, T0 + 3 * MIN, schema=m.SCHEMA_START)))
    assert v[0] == Verdict.GO


def test_win_without_a_session(env):
    _, _, svc, _ = env
    _, v = verdict_of(svc.uplink(record(0x201, RED, T0 + 1)))
    assert v[0] == Verdict.NO_SESSION


def test_repeated_delivery_is_ignored(env):
    game, store, svc, _ = env
    svc.uplink(record(0x201, RED, T0, schema=m.SCHEMA_START))
    raw = record(0x201, RED, T0 + 1, schema=m.SCHEMA_WRONG, fcnt=500)
    assert svc.uplink(raw) == []
    assert svc.uplink(raw) == []
    assert store.load(game, "r1").teams["red"].wrong == {"vault": 1}


def test_insecure_refused_unless_allowed(env, tmp_path):
    game, store, svc, _ = env
    start = dict(schema=m.SCHEMA_START)
    assert svc.uplink(record(0x201, RED, T0 + 1, sec="auth", **start)) == []
    assert svc.uplink(record(0x201, RED, T0 + 1, sec="none", **start)) == []
    bench = Service(game, store, ServiceConfig(allow_insecure=True))
    _, v = verdict_of(bench.uplink(record(0x201, RED, T0 + 1, sec="none", **start)))
    assert v[0] == Verdict.GO


def test_strays_are_ignored(env):
    _, store, svc, _ = env
    assert svc.uplink(record(0x2FF, RED, T0 + 1)) == []  # not a box
    assert svc.uplink(record(0x201, RED, T0 + 1, schema=40)) == []  # not a game schema
    assert svc.uplink(b"{not json") == []
    bad = json.loads(record(0x201, RED, T0 + 1))
    bad["payload"] = "0205"
    assert svc.uplink(json.dumps(bad).encode()) == []
    assert svc.uplink(record(0x201, RED, T0 + 1, schema=m.SCHEMA_HINT)) == []  # no hint number
    assert [e["kind"] for e in store.events("r1")] == ["team_entered", "team_entered", "run_started"]


def test_unknown_team_is_recorded(env):
    _, store, svc, _ = env
    _, v = verdict_of(svc.uplink(record(0x201, b"\x77", T0 + 1, schema=m.SCHEMA_START)))
    assert v[0] == Verdict.UNKNOWN_TEAM
    assert store.events("r1")[-1]["verdict"] == Verdict.UNKNOWN_TEAM
    assert svc.uplink(record(0x201, b"\x77", T0 + 2, schema=m.SCHEMA_WRONG)) == []
    ev = store.events("r1")[-1]
    assert ev["kind"] == "wrong" and ev["team"] is None and not ev["counted"]


def test_finish_and_restart_rebuilds_state(env, tmp_path):
    game, store, svc, _ = env
    for i, node in enumerate([0x201, 0x203, 0x202]):
        play(svc, node, RED, T0 + i * MIN)
    _, v = play(svc, 0x204, RED, T0 + 10 * MIN)
    assert v[:3] == (Verdict.FINISHED, 4, 4)
    again = Service(game, Store(str(tmp_path / "game.db")), ServiceConfig())
    red = again.run.teams["red"]
    assert red.finished_at == T0 + 10 * MIN + S
    assert again.run.time_ms(red) == 4 * S
    assert again.run.standings()[0]["team"] == "red"


def test_deadline_ends_the_run(env):
    game, store, svc, clock = env
    verdict_of(svc.uplink(record(0x201, RED, T0 + 59 * MIN, schema=m.SCHEMA_START)))
    clock[0] = T0 + 3600_000
    svc.tick()
    assert svc.run.state == "ended" and svc.run.ended_at == T0 + 3600_000
    assert svc.run.teams["red"].box_time["vault"] == MIN  # the open session closed at the end
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
    _, v = play(svc, 0x202, RED, T0 + 2)  # lab needs vault
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
    _, v = verdict_of(svc.uplink(record(0x201, RED, T0, schema=m.SCHEMA_START)))
    assert v[0] == Verdict.NOT_RUNNING
    assert svc.uplink(record(0x201, RED, T0, schema=m.SCHEMA_WRONG)) == []


def test_api_views(env):
    game, store, svc, _ = env
    play(svc, 0x201, RED, T0)
    svc.uplink(record(0x203, BLUE, T0 + 5 * S, schema=m.SCHEMA_START))
    view = api.run_view(game, store, now=T0 + 10 * S)
    red = next(t for t in view["run"]["teams"] if t["team"] == "red")
    assert red["name"] == "Red" and red["solved"] == {"vault": T0 + S}
    assert red["open"] == ["lab", "garden", "cellar"]
    assert view["run"]["sessions"] == {"garden": {"team": "blue", "start": T0 + 5 * S, "last": T0 + 5 * S}}
    st = api.standings(game, store, now=T0 + 10 * S)
    assert [(x["team"], x["points"], x["time_ms"]) for x in st] == [("red", 100, S), ("blue", 0, 5 * S)]


def test_api_over_http(env, tmp_path):
    import urllib.request
    from http.server import ThreadingHTTPServer

    game, store, svc, _ = env
    play(svc, 0x201, RED, T0)
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
        assert json.loads(get("/api/events?after=3"))[1]["kind"] == "claim"
        assert b"<title>Scoreboard</title>" in get("/")
    finally:
        servers[0].shutdown()


def test_cli(tmp_path, capsys, monkeypatch):
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

    # a box reset closes the box's open session, then tells the box
    game = parse_game(__import__("tomllib").loads(gfile.read_text()))
    store = Store(db)
    live = store.load(game, "evening")
    with store.change(live) as append:
        append({"kind": "start", "at": service.now_ms(), "box": "garden", "team": "red",
                "verdict": int(Verdict.GO)})
    sent = []

    class FakeClient:
        def publish(self, topic, body, qos):
            sent.append((topic, json.loads(body)))
            return type("Info", (), {"wait_for_publish": lambda self, timeout: None,
                                     "is_published": lambda self: True})()

        def loop_stop(self): pass
        def disconnect(self): pass

    monkeypatch.setattr(service, "connect", lambda *a, **k: FakeClient())
    assert run("control", "garden", "reset") == 0
    assert sent[0][1]["schema"] == m.SCHEMA_CONTROL and sent[0][1]["node"] == "0x0203"
    assert store.events("evening")[-1]["kind"] == "session_closed"
    assert run("end") == 0
    r = Store(db).load(game, "evening")
    assert r.state == "ended" and r.tokens[b"RED7"] == "red" and "vault" in r.teams["red"].solved


def test_box_data_from_the_hub(env):
    game, *_ = env
    asked = []

    def hub(url):
        asked.append(url)
        if "node=513" not in url:  # only the vault (0x201) has sent any
            return []
        return [  # newest first, as the hub's /api/uplinks answers
            {"schema": 2, "received_at": 3, "payload": "0103524544"},
            {"schema": 32, "received_at": 2, "payload": "a708", "decoded": {"temp_c": 22.15},
             "decode_error": None},
        ]

    data = api.box_data(game, "http://hub:8080/", get=hub)
    latest = {"received_at": 2, "schema": 32, "payload": "a708", "decoded": {"temp_c": 22.15},
              "decode_error": None}
    assert data == {"vault": {**latest, "recent": [latest]}}
    assert asked[0] == "http://hub:8080/api/uplinks?net=66&node=513&min_schema=32&limit=10"
    assert len(asked) == len(game.boxes)
