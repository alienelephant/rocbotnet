"""The game's rules, with no I/O (docs/GAME.md §3 and §4).

A `Game` is a definition: boxes, prerequisites, points and penalties, which
boxes are required, a session timeout and a time limit. A `Run` is one
playing of it. Every change to a run is an event (a plain dict);
`Run.apply()` is the only thing that changes state, so a run can be rebuilt
by replaying its events from the log in order. The `judge_*` methods decide
a box message's verdict without changing anything; the caller stores the
verdict in an event and applies it.
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
    points: int = 100
    hint_penalties_s: tuple[int, ...] = ()  # hint i costs hint_penalties_s[i-1]
    wrong_penalty_s: int = 0


@dataclass(frozen=True)
class Game:
    id: str
    title: str
    boxes: dict[str, Box]  # in declaration order
    time_limit_s: int | None = None
    session_timeout_s: int = 20 * 60
    # Any fob plays (docs/GAME.md §5): a START from a token no team holds
    # enters a team for it first, named from open_team_name. For running a
    # game before there's a registration desk or a backend database.
    open_teams: bool = False
    open_team_name: str = "Fob {first3}"

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
                points=int(b.get("points", g.get("points", 100))),
                hint_penalties_s=tuple(int(p) for p in b.get("hint_penalty_s", g.get("hint_penalty_s", []))),
                wrong_penalty_s=int(b.get("wrong_penalty_s", g.get("wrong_penalty_s", 0))),
            )
        except (KeyError, TypeError, ValueError) as e:
            raise GameError(f"[[box]] {b.get('id', '?')}: {e}") from None
        if box.id in boxes:
            raise GameError(f"box {box.id} is defined twice")
        if not 0x0001 <= box.node <= 0xFEFF:
            raise GameError(f"box {box.id}: node 0x{box.node:04X} isn't a roclink node address")
        if box.points < 0 or box.wrong_penalty_s < 0 or any(p < 0 for p in box.hint_penalties_s):
            raise GameError(f"box {box.id}: points and penalties can't be negative")
        if list(box.hint_penalties_s) != sorted(box.hint_penalties_s):
            raise GameError(f"box {box.id}: each hint must cost at least as much as the one before")
        if len(box.hint_penalties_s) > 255:
            raise GameError(f"box {box.id}: at most 255 hints")
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
    timeout = g.get("session_timeout_min", 20)
    if not timeout or timeout <= 0:
        raise GameError("session_timeout_min must be positive")
    open_teams = g.get("open_teams", False)
    if not isinstance(open_teams, bool):
        raise GameError("open_teams must be true or false")
    open_name = g.get("open_team_name", "Fob {first3}")
    try:
        open_team_label(open_name, bytes(5))
    except (KeyError, IndexError, ValueError, TypeError, AttributeError) as e:
        raise GameError(f"open_team_name {open_name!r}: {e!r} (use {{first3}}, {{last3}}, {{hex}}, {{number}})") from None
    game = Game(g["id"], g.get("title", g["id"]), boxes,
                int(limit * 60) if limit else None, int(timeout * 60), open_teams, open_name)
    if not game.required:
        raise GameError("at least one box must be required")
    return game


def open_team_label(template: str, token: bytes) -> str:
    """A team's name from its fob, for open_teams. The placeholders:
    {first3} and {last3}, the first and last three hex digits of the token
    ("0F0", "2C3" for 0F00A1B2C3); {hex}, all of it; {number}, its low 32 bits
    in decimal, the number EM4100 fobs usually have printed on them."""
    h = token.hex().upper()
    return template.format(first3=h[:3], last3=h[-3:], hex=h,
                           number=int.from_bytes(token[-4:], "big"))


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
    GO = 6
    BUSY = 7
    NO_SESSION = 8
    HINT = 9
    NO_MORE_HINTS = 10

    @property
    def accepted(self) -> bool:
        return self in (Verdict.ACCEPTED, Verdict.FINISHED)


@dataclass
class Session:
    team: str
    start: int
    last: int  # the time of its latest message


@dataclass
class Team:
    id: str
    name: str
    tokens: set[bytes] = field(default_factory=set)
    solved: dict[str, int] = field(default_factory=dict)  # box id -> hub time (ms)
    finished_at: int | None = None
    box_time: dict[str, int] = field(default_factory=dict)  # box id -> ms in closed sessions
    hints: dict[str, int] = field(default_factory=dict)  # box id -> hints taken
    wrong: dict[str, int] = field(default_factory=dict)  # box id -> wrong attempts in sessions
    penalty_ms: int = 0


@dataclass(frozen=True)
class Reply:
    """A verdict, and what the box is told with it."""

    verdict: Verdict
    team: str | None
    box: str
    solved_required: int
    required_total: int
    needs: str | None = None  # LOCKED: a box to solve first
    arg: int = 0  # GO: hints taken; BUSY: seconds left; HINT: its number; NO_MORE_HINTS: how many


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
        self.sessions: dict[str, Session] = {}  # box id -> its open session
        self.last_event = 0  # the id of the last stored event applied (store.py)

    # --------------------------------------------------------- queries

    @property
    def deadline(self) -> int | None:
        if self.started_at is None or self.game.time_limit_s is None:
            return None
        return self.started_at + self.game.time_limit_s * 1000

    @property
    def timeout_ms(self) -> int:
        return self.game.session_timeout_s * 1000

    def running_at(self, now: int) -> bool:
        return self.state == "running" and (self.deadline is None or now < self.deadline)

    def session_at(self, box_id: str, now: int) -> Session | None:
        """The box's open session, unless it has timed out by `now`."""
        s = self.sessions.get(box_id)
        return s if s is not None and now < s.last + self.timeout_ms else None

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

    def points(self, team: Team) -> int:
        return sum(self.game.boxes[b].points for b in team.solved)

    def time_ms(self, team: Team, now: int | None = None) -> int:
        """Box time plus penalties. With `now`, open sessions count up to it."""
        t = sum(team.box_time.values()) + team.penalty_ms
        if now is not None:
            for s in self.sessions.values():
                if s.team == team.id:
                    t += max(0, min(now, s.last + self.timeout_ms) - s.start)
        return t

    def standings(self, now: int | None = None) -> list[dict]:
        """docs/GAME.md §3: finished first, then points, then time, then the
        time of the last solve."""
        required = set(self.game.required)

        def key(t: Team):
            last = max(t.solved.values(), default=float("inf"))
            return (t.finished_at is None, -self.points(t), self.time_ms(t, now), last)

        ranked = sorted(self.teams.values(), key=key)
        return [
            {
                "rank": i + 1,
                "team": t.id,
                "name": t.name,
                "points": self.points(t),
                "time_ms": self.time_ms(t, now),
                "solved": sorted(t.solved, key=lambda b: t.solved[b]),
                "required_solved": self.required_solved(t),
                "required_total": len(required),
                "hints": sum(t.hints.values()),
                "wrong": sum(t.wrong.values()),
                "finished_at": t.finished_at,
            }
            for i, t in enumerate(ranked)
        ]

    def winner(self) -> str | None:
        s = self.standings()
        return s[0]["team"] if s and s[0]["finished_at"] is not None else None

    # --------------------------------------------- judging a box's message

    def _who(self, token: bytes, box: Box, now: int) -> Reply | Team:
        """The team behind a token, or the reply that refuses it outright."""
        total = len(self.game.required)
        if not self.running_at(now):
            return Reply(Verdict.NOT_RUNNING, None, box.id, 0, total)
        tid = self.tokens.get(token)
        if tid is None:
            return Reply(Verdict.UNKNOWN_TEAM, None, box.id, 0, total)
        return self.teams[tid]

    def _reply(self, v: Verdict, team: Team, box: Box, **kw) -> Reply:
        return Reply(v, team.id, box.id, self.required_solved(team), len(self.game.required), **kw)

    def judge_start(self, token: bytes, box: Box, now: int) -> Reply:
        team = self._who(token, box, now)
        if isinstance(team, Reply):
            return team
        if box.id in team.solved:
            return self._reply(Verdict.ALREADY_SOLVED, team, box)
        if not self.is_open_for(team, box):
            return self._reply(Verdict.LOCKED, team, box, needs=self.first_missing(team, box))
        s = self.session_at(box.id, now)
        if s is not None and s.team != team.id:
            left = -(-(s.last + self.timeout_ms - now) // 1000)  # round up
            return self._reply(Verdict.BUSY, team, box, arg=min(left, 0xFFFF))
        return self._reply(Verdict.GO, team, box, arg=team.hints.get(box.id, 0))

    def judge_win(self, token: bytes, box: Box, now: int) -> Reply:
        """§3 conditions 1-6."""
        team = self._who(token, box, now)
        if isinstance(team, Reply):
            return team
        if box.id in team.solved:
            return self._reply(Verdict.ALREADY_SOLVED, team, box)
        if not self.is_open_for(team, box):
            return self._reply(Verdict.LOCKED, team, box, needs=self.first_missing(team, box))
        if not self._in_session(team, box, now):
            return self._reply(Verdict.NO_SESSION, team, box)
        finishes = team.finished_at is None and all(
            b in team.solved or b == box.id for b in self.game.required
        )
        done = self.required_solved(team) + (1 if box.required else 0)
        return Reply(Verdict.FINISHED if finishes else Verdict.ACCEPTED, team.id, box.id, done,
                     len(self.game.required))

    def judge_hint(self, token: bytes, box: Box, n: int, now: int) -> Reply:
        team = self._who(token, box, now)
        if isinstance(team, Reply):
            return team
        if not self._in_session(team, box, now):
            return self._reply(Verdict.NO_SESSION, team, box)
        taken = team.hints.get(box.id, 0)
        if n < 1 or n > len(box.hint_penalties_s) or n > taken + 1:
            return self._reply(Verdict.NO_MORE_HINTS, team, box, arg=len(box.hint_penalties_s))
        return self._reply(Verdict.HINT, team, box, arg=n)

    def in_session(self, token: bytes, box: Box, now: int) -> str | None:
        """For WRONG and END, which get no reply: the team, if it has the session."""
        tid = self.tokens.get(token)
        if tid is None or not self.running_at(now):
            return None
        return tid if self._in_session(self.teams[tid], box, now) else None

    def _in_session(self, team: Team, box: Box, now: int) -> bool:
        s = self.session_at(box.id, now)
        return s is not None and s.team == team.id

    # ----------------------------------------------------------- events

    def apply(self, ev: dict) -> None:
        kind, at = ev["kind"], ev["at"]
        self._expire(at)
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
            self._end(at)
        elif kind == "start":
            if ev["verdict"] == Verdict.GO:
                s = self.sessions.get(ev["box"])
                if s is not None and s.team == ev["team"]:
                    s.last = at
                else:
                    self.sessions[ev["box"]] = Session(ev["team"], at, at)
        elif kind == "claim":
            if Verdict(ev["verdict"]).accepted:
                self._close(ev["box"], at)
                self._solve(ev["team"], ev["box"], at)
        elif kind == "hint":
            if ev["verdict"] == Verdict.HINT:
                team, box = self.teams[ev["team"]], self.game.boxes[ev["box"]]
                n = ev["n"]
                if n > team.hints.get(box.id, 0):
                    team.hints[box.id] = n
                    team.penalty_ms += box.hint_penalties_s[n - 1] * 1000
                self._touch(ev["box"], at)
        elif kind == "wrong":
            if ev.get("team") and ev.get("counted"):
                team, box = self.teams[ev["team"]], self.game.boxes[ev["box"]]
                team.wrong[box.id] = team.wrong.get(box.id, 0) + 1
                team.penalty_ms += box.wrong_penalty_s * 1000
                self._touch(ev["box"], at)
        elif kind == "end":
            if ev.get("team") and self.sessions.get(ev["box"]) and self.sessions[ev["box"]].team == ev["team"]:
                self._close(ev["box"], at)
        elif kind == "session_closed":  # the operator
            self._close(ev["box"], at)
        elif kind == "correction":
            team = self.teams[ev["team"]]
            if ev["action"] == "add":
                self._solve(ev["team"], ev["box"], at)
            else:
                team.solved.pop(ev["box"], None)
                self._update_finish(team)
        else:
            raise GameError(f"unknown event {kind}")
        if kind in ("claim", "correction") and self.state == "running" and self.teams and all(
            t.finished_at is not None for t in self.teams.values()
        ):
            self._end(at)  # every team finished (§3)

    def _expire(self, now: int) -> None:
        """Close sessions that timed out before `now`, charging the full timeout."""
        for bid, s in list(self.sessions.items()):
            if now >= s.last + self.timeout_ms:
                self._close(bid, s.last + self.timeout_ms)

    def _touch(self, box_id: str, at: int) -> None:
        s = self.sessions.get(box_id)
        if s is not None:
            s.last = max(s.last, at)

    def _close(self, box_id: str, at: int) -> None:
        s = self.sessions.pop(box_id, None)
        if s is not None:
            team = self.teams[s.team]
            team.box_time[box_id] = team.box_time.get(box_id, 0) + max(0, at - s.start)

    def _end(self, at: int) -> None:
        for bid in list(self.sessions):
            self._close(bid, at)
        self.state, self.ended_at = "ended", at

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
