"""The game's rules, with no I/O (docs/GAME.md §3 and §4).

A `Game` is a definition: boxes, prerequisites, which boxes are required, a
time limit. A `Run` is one playing of it. Every change to a run is an event
(a plain dict); `Run.apply()` is the only thing that changes state, so a run
can be rebuilt by replaying its events from the log in order.
"""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from enum import IntEnum


class GameError(ValueError):
    pass


# ------------------------------------------------------------------ game


@dataclass(frozen=True)
class Box:
    id: str
    name: str
    net: int
    node: int
    requires: tuple[str, ...] = ()
    required: bool = True


@dataclass(frozen=True)
class Game:
    id: str
    title: str
    boxes: dict[str, Box]  # in declaration order
    time_limit_s: int | None = None

    @property
    def required(self) -> list[str]:
        return [b.id for b in self.boxes.values() if b.required]

    def box_at(self, net: int, node: int) -> Box | None:
        for b in self.boxes.values():
            if (b.net, b.node) == (net, node):
                return b
        return None


def load_game(path: str) -> Game:
    with open(path, "rb") as f:
        raw = tomllib.load(f)
    return parse_game(raw)


def parse_game(raw: dict) -> Game:
    g = raw.get("game", {})
    if not isinstance(g.get("id"), str):
        raise GameError("[game] id is required")
    boxes: dict[str, Box] = {}
    for b in raw.get("box", []):
        try:
            box = Box(
                id=b["id"],
                name=b.get("name", b["id"]),
                net=int(b["net"]),
                node=int(b["node"]),
                requires=tuple(b.get("requires", [])),
                required=bool(b.get("required", True)),
            )
        except (KeyError, TypeError, ValueError) as e:
            raise GameError(f"[[box]] {b.get('id', '?')}: {e}") from None
        if box.id in boxes:
            raise GameError(f"box {box.id} is defined twice")
        if not 0x0001 <= box.node <= 0xFEFF:
            raise GameError(f"box {box.id}: node 0x{box.node:04X} isn't a roclink node address")
        boxes[box.id] = box
    if not boxes:
        raise GameError("a game needs at least one [[box]]")
    addresses = [(b.net, b.node) for b in boxes.values()]
    if len(set(addresses)) != len(addresses):
        raise GameError("two boxes share a roclink address")
    for b in boxes.values():
        for r in b.requires:
            if r not in boxes:
                raise GameError(f"box {b.id} requires unknown box {r}")
    _check_acyclic(boxes)
    limit = g.get("time_limit_min")
    game = Game(g["id"], g.get("title", g["id"]), boxes, int(limit * 60) if limit else None)
    if not game.required:
        raise GameError("at least one box must be required")
    return game


def _check_acyclic(boxes: dict[str, Box]) -> None:
    state: dict[str, int] = {}  # 1 visiting, 2 done

    def visit(bid: str, path: list[str]) -> None:
        if state.get(bid) == 2:
            return
        if state.get(bid) == 1:
            raise GameError("prerequisites form a cycle: " + " -> ".join(path + [bid]))
        state[bid] = 1
        for r in boxes[bid].requires:
            visit(r, path + [bid])
        state[bid] = 2

    for bid in boxes:
        visit(bid, [])


# ------------------------------------------------------------------- run


class Verdict(IntEnum):
    """The `result` byte of a VERDICT downlink (docs/GAME.md §6)."""

    ACCEPTED = 0
    FINISHED = 1
    ALREADY_SOLVED = 2
    LOCKED = 3
    UNKNOWN_TEAM = 4
    NOT_RUNNING = 5

    @property
    def accepted(self) -> bool:
        return self in (Verdict.ACCEPTED, Verdict.FINISHED)


@dataclass
class Team:
    id: str
    name: str
    tokens: set[bytes] = field(default_factory=set)
    solved: dict[str, int] = field(default_factory=dict)  # box id -> hub time (ms)
    finished_at: int | None = None


@dataclass(frozen=True)
class ClaimResult:
    verdict: Verdict
    team: str | None
    box: str
    solved_required: int
    required_total: int
    needs: str | None = None  # for LOCKED: a box to solve first


class Run:
    """One playing of a game. State changes only through apply(event)."""

    def __init__(self, game: Game, run_id: str):
        self.game = game
        self.id = run_id
        self.state = "open"  # open -> running -> ended
        self.started_at: int | None = None
        self.ended_at: int | None = None
        self.teams: dict[str, Team] = {}
        self.tokens: dict[bytes, str] = {}
        self.last_event = 0  # the id of the last stored event applied (store.py)

    # --------------------------------------------------------- queries

    @property
    def deadline(self) -> int | None:
        if self.started_at is None or self.game.time_limit_s is None:
            return None
        return self.started_at + self.game.time_limit_s * 1000

    def running_at(self, now: int) -> bool:
        return self.state == "running" and (self.deadline is None or now < self.deadline)

    def is_open_for(self, team: Team, box: Box) -> bool:
        return all(r in team.solved for r in box.requires)

    def first_missing(self, team: Team, box: Box) -> str | None:
        """A prerequisite to solve first: the earliest one, in declaration order, that's open."""
        missing = [r for r in self.game.boxes if r in self._closure(box.id) and r not in team.solved]
        for r in missing:
            if self.is_open_for(team, self.game.boxes[r]):
                return r
        return missing[0] if missing else None

    def _closure(self, bid: str) -> set[str]:
        out: set[str] = set()
        stack = list(self.game.boxes[bid].requires)
        while stack:
            r = stack.pop()
            if r not in out:
                out.add(r)
                stack.extend(self.game.boxes[r].requires)
        return out

    def required_solved(self, team: Team) -> int:
        return sum(1 for b in self.game.required if b in team.solved)

    def standings(self) -> list[dict]:
        """docs/GAME.md §3: finished first by finish time, then by required
        boxes solved, then by last solve time, then optional boxes solved."""
        required = set(self.game.required)

        def key(t: Team):
            last = max(t.solved.values(), default=float("inf"))
            optional = sum(1 for b in t.solved if b not in required)
            if t.finished_at is not None:
                return (0, t.finished_at, 0, 0, 0)
            return (1, 0, -self.required_solved(t), last, -optional)

        ranked = sorted(self.teams.values(), key=key)
        return [
            {
                "rank": i + 1,
                "team": t.id,
                "name": t.name,
                "solved": sorted(t.solved, key=lambda b: t.solved[b]),
                "required_solved": self.required_solved(t),
                "required_total": len(required),
                "finished_at": t.finished_at,
            }
            for i, t in enumerate(ranked)
        ]

    def winner(self) -> str | None:
        s = self.standings()
        return s[0]["team"] if s and s[0]["finished_at"] is not None else None

    # ------------------------------------------------- deciding a claim

    def judge(self, token: bytes, box: Box, now: int) -> ClaimResult:
        """The verdict for a WIN, without changing anything (§3 conditions 1-5)."""
        total = len(self.game.required)
        if not self.running_at(now):
            return ClaimResult(Verdict.NOT_RUNNING, None, box.id, 0, total)
        tid = self.tokens.get(token)
        if tid is None:
            return ClaimResult(Verdict.UNKNOWN_TEAM, None, box.id, 0, total)
        team = self.teams[tid]
        done = self.required_solved(team)
        if box.id in team.solved:
            return ClaimResult(Verdict.ALREADY_SOLVED, tid, box.id, done, total)
        if not self.is_open_for(team, box):
            return ClaimResult(Verdict.LOCKED, tid, box.id, done, total, self.first_missing(team, box))
        done += 1 if box.required else 0
        finishes = team.finished_at is None and all(
            b in team.solved or b == box.id for b in self.game.required
        )
        return ClaimResult(Verdict.FINISHED if finishes else Verdict.ACCEPTED, tid, box.id, done, total)

    # ----------------------------------------------------------- events

    def apply(self, ev: dict) -> None:
        kind, at = ev["kind"], ev["at"]
        if kind == "team_entered":
            if self.state == "ended":
                raise GameError("the run has ended")
            if ev["team"] in self.teams:
                raise GameError(f"team {ev['team']} is already in the run")
            tokens = {bytes.fromhex(t) for t in ev["tokens"]}
            taken = tokens & set(self.tokens)
            if taken:
                raise GameError("token already issued: " + ", ".join(t.hex() for t in taken))
            self.teams[ev["team"]] = Team(ev["team"], ev.get("name", ev["team"]), tokens)
            for t in tokens:
                self.tokens[t] = ev["team"]
        elif kind == "run_started":
            if self.state != "open":
                raise GameError(f"the run is {self.state}")
            self.state, self.started_at = "running", at
        elif kind == "run_ended":
            if self.state == "ended":
                raise GameError("the run has already ended")
            self.state, self.ended_at = "ended", at
        elif kind == "claim":
            if Verdict(ev["verdict"]).accepted:
                self._solve(ev["team"], ev["box"], at)
        elif kind == "correction":
            team = self.teams[ev["team"]]
            if ev["action"] == "add":
                self._solve(ev["team"], ev["box"], at)
            else:
                team.solved.pop(ev["box"], None)
                self._update_finish(team)
        elif kind == "attempt":
            pass  # recorded for the history, no effect on state
        else:
            raise GameError(f"unknown event {kind}")
        if kind in ("claim", "correction") and self.state == "running" and self.teams and all(
            t.finished_at is not None for t in self.teams.values()
        ):
            self.state, self.ended_at = "ended", at  # every team finished (§3)

    def _solve(self, tid: str, bid: str, at: int) -> None:
        team = self.teams[tid]
        team.solved.setdefault(bid, at)
        self._update_finish(team)

    def _update_finish(self, team: Team) -> None:
        required = self.game.required
        if all(b in team.solved for b in required):
            team.finished_at = max(team.solved[b] for b in required)
        else:
            team.finished_at = None
