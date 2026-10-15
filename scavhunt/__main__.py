"""The operator's commands. Run `python -m scavhunt -h`.

    scavhunt check                      is the game file valid? which boxes open first?
    scavhunt new-run [ID]               create a run (it becomes the current one)
    scavhunt team ID --token T [...]    enter a team and issue its tokens
    scavhunt start | end                start or end the current run
    scavhunt solve TEAM BOX             correct: add a solve (and unsolve to remove one)
    scavhunt status                     the current run's standings
    scavhunt control BOX reset|disable|enable
    scavhunt serve                      the hub service and the scoreboard
"""

from __future__ import annotations

import argparse
import logging
import os
import signal
import sys
import threading
import time
import tomllib

from . import api, service
from . import messages as msg
from .game import GameError, parse_game
from .store import Store

log = logging.getLogger("scavhunt")


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="scavhunt", description="The roclink scavenger hunt.")
    p.add_argument("-g", "--game", default=os.environ.get("SCAVHUNT_GAME", "game.toml"),
                   help="the game file (default game.toml, or $SCAVHUNT_GAME)")
    p.add_argument("--db", help="the event log (default: [service] db in the game file, or game.db)")
    p.add_argument("-v", "--verbose", action="store_true")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check")
    s = sub.add_parser("new-run")
    s.add_argument("run_id", nargs="?")
    s = sub.add_parser("team")
    s.add_argument("team")
    s.add_argument("--name")
    s.add_argument("--token", action="append", required=True,
                   help="a card UID in hex (04a1b2c3) or a typed code (text:RED7); repeat for more")
    sub.add_parser("start")
    sub.add_parser("end")
    for name in ("solve", "unsolve"):
        s = sub.add_parser(name)
        s.add_argument("team")
        s.add_argument("box")
        s.add_argument("--note", default="")
    sub.add_parser("status")
    s = sub.add_parser("control")
    s.add_argument("box")
    s.add_argument("command", choices=["reset", "disable", "enable"])
    sub.add_parser("serve")
    a = p.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    try:
        return run_command(a)
    except (GameError, msg.MessageError, OSError, tomllib.TOMLDecodeError) as e:
        print(f"scavhunt: {e}", file=sys.stderr)
        return 1


def run_command(a) -> int:
    with open(a.game, "rb") as f:
        raw = tomllib.load(f)
    game = parse_game(raw)
    scfg = service_config(raw.get("service", {}))
    if a.cmd == "check":
        return check(game)
    store = Store(a.db or raw.get("service", {}).get("db", "game.db"))
    now = service.now_ms()

    if a.cmd == "new-run":
        rid = a.run_id or time.strftime("%Y%m%d-%H%M%S")
        store.create_run(game, rid, now)
        print(f"run {rid} created; enter teams, then start it")
        return 0
    if a.cmd == "serve":
        return serve(game, store, scfg, raw.get("api", {}), a.db or raw.get("service", {}).get("db", "game.db"))
    if a.cmd == "control":
        topic, body = service.Service(game, store, scfg).control(
            a.box, {"reset": msg.CONTROL_RESET, "disable": msg.CONTROL_DISABLE,
                    "enable": msg.CONTROL_ENABLE}[a.command])
        client = service.connect(scfg, f"{scfg.name}-control")
        info = client.publish(topic, body, qos=1)
        info.wait_for_publish(timeout=10)
        client.loop_stop()
        client.disconnect()
        print(f"{a.command} sent to {a.box}" if info.is_published() else "the broker didn't take it")
        return 0 if info.is_published() else 1

    rid = store.current_run_id(game)
    if rid is None:
        raise GameError("no run yet: scavhunt new-run")
    run = store.load(game, rid)
    if a.cmd == "status":
        print(f"{game.title}, run {run.id}: {run.state}")
        for s in run.standings():
            fin = f"finished at +{(s['finished_at'] - run.started_at) / 1000:.0f}s" if s["finished_at"] else ""
            print(f"{s['rank']:>3}. {s['name']:<24} {s['required_solved']}/{s['required_total']}  "
                  f"{', '.join(s['solved'])}  {fin}")
        return 0
    with store.change(run) as append:
        if a.cmd == "team":
            tokens = [msg.parse_token(t).hex() for t in a.token]
            append({"kind": "team_entered", "at": now, "team": a.team, "name": a.name or a.team,
                    "tokens": tokens})
        elif a.cmd == "start":
            append({"kind": "run_started", "at": now})
        elif a.cmd == "end":
            append({"kind": "run_ended", "at": now, "reason": "operator"})
        else:  # solve / unsolve
            if a.team not in run.teams:
                raise GameError(f"no team {a.team} in this run")
            if a.box not in game.boxes:
                raise GameError(f"no box {a.box}")
            append({"kind": "correction", "at": now, "team": a.team, "box": a.box,
                    "action": "add" if a.cmd == "solve" else "remove", "note": a.note})
    print(f"run {run.id}: {a.cmd} done ({run.state})")
    return 0


def service_config(raw: dict) -> service.ServiceConfig:
    known = {f for f in service.ServiceConfig.__dataclass_fields__}
    kw = {k: v for k, v in raw.items() if k in known}
    if "password_file" in raw:
        with open(raw["password_file"]) as f:
            kw["password"] = f.read().strip()
    return service.ServiceConfig(**kw)


def check(game) -> int:
    print(f"{game.title} ({game.id}): {len(game.boxes)} boxes, {len(game.required)} required"
          + (f", {game.time_limit_s // 60} min" if game.time_limit_s else ""))
    for b in game.boxes.values():
        needs = f" after {', '.join(b.requires)}" if b.requires else " (open at the start)"
        print(f"  {b.id:<12} 0x{b.net:02X}/0x{b.node:04X}{'' if b.required else ' optional'}{needs}")
    return 0


def serve(game, store, scfg, api_cfg, db_path) -> int:
    stop = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.set())
    if api_cfg.get("enabled", True):
        threading.Thread(
            target=api.serve,
            args=(game, db_path, api_cfg.get("host", "127.0.0.1"), int(api_cfg.get("port", 8090))),
            daemon=True,
        ).start()
    service.run(game, store, scfg, stop)
    return 0


if __name__ == "__main__":
    sys.exit(main())
