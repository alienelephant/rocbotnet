import pytest

from scavhunt.game import GameError, Run, Verdict, parse_game

GAME = {
    "game": {"id": "g", "title": "Test", "time_limit_min": 60, "session_timeout_min": 10,
             "hint_penalty_s": [60, 120], "wrong_penalty_s": 30},
    "box": [
        {"id": "vault", "net": 0x42, "node": 0x201},
        {"id": "lab", "net": 0x42, "node": 0x202, "requires": ["vault"]},
        {"id": "garden", "net": 0x42, "node": 0x203},
        {"id": "tower", "net": 0x42, "node": 0x204, "requires": ["lab", "garden"], "points": 200},
        {"id": "cellar", "net": 0x42, "node": 0x205, "required": False, "points": 50,
         "hint_penalty_s": []},
    ],
}
RED, BLUE = b"\x04\xa1", b"\x04\xb2"
T0 = 1_000_000
S, MIN = 1000, 60_000  # ms


def make_run(*teams):
    game = parse_game(GAME)
    run = Run(game, "r1")
    for tid, tok in teams or (("red", RED), ("blue", BLUE)):
        run.apply({"kind": "team_entered", "at": 0, "team": tid, "tokens": [tok.hex()]})
    run.apply({"kind": "run_started", "at": T0})
    return run


def start(run, token, box_id, at):
    r = run.judge_start(token, run.game.boxes[box_id], at)
    run.apply({"kind": "start", "at": at, "box": box_id, "team": r.team, "verdict": int(r.verdict)})
    return r


def win(run, token, box_id, at):
    r = run.judge_win(token, run.game.boxes[box_id], at)
    run.apply({"kind": "claim", "at": at, "box": box_id, "team": r.team, "verdict": int(r.verdict)})
    return r


def hint(run, token, box_id, n, at):
    r = run.judge_hint(token, run.game.boxes[box_id], n, at)
    run.apply({"kind": "hint", "at": at, "box": box_id, "n": n, "team": r.team, "verdict": int(r.verdict)})
    return r


def wrong(run, token, box_id, at):
    team = run.in_session(token, run.game.boxes[box_id], at)
    run.apply({"kind": "wrong", "at": at, "box": box_id, "team": team, "counted": team is not None})


def play(run, token, box_id, at, took=1 * S):
    assert start(run, token, box_id, at).verdict == Verdict.GO
    return win(run, token, box_id, at + took)


# ---------------------------------------------------------------- the game file


def test_parse():
    g = parse_game(GAME)
    assert g.required == ["vault", "lab", "garden", "tower"]
    assert g.time_limit_s == 3600 and g.session_timeout_s == 600
    assert g.box_at(0x42, 0x204).id == "tower"
    assert g.box_at(0x43, 0x204) is None
    assert g.boxes["vault"].hint_penalties_s == (60, 120)  # the game's defaults
    assert g.boxes["cellar"].hint_penalties_s == ()  # a box's own
    assert (g.boxes["vault"].points, g.boxes["tower"].points) == (100, 200)


@pytest.mark.parametrize(
    "boxes, error",
    [
        ([], "at least one"),
        ([{"id": "a", "net": 1, "node": 1, "requires": ["a"]}], "cycle"),
        ([{"id": "a", "net": 1, "node": 1, "requires": ["b"]},
          {"id": "b", "net": 1, "node": 2, "requires": ["a"]}], "cycle"),
        ([{"id": "a", "net": 1, "node": 1, "requires": ["x"]}], "unknown box"),
        ([{"id": "a", "net": 1, "node": 1}, {"id": "a", "net": 1, "node": 2}], "twice"),
        ([{"id": "a", "net": 1, "node": 1}, {"id": "b", "net": 1, "node": 1}], "share"),
        ([{"id": "a", "net": 1, "node": 0xFFFF}], "node address"),
        ([{"id": "a", "net": 1, "node": 1, "required": False}], "required"),
        ([{"id": "a", "net": 1}], "node"),
        ([{"id": "a", "net": 1, "node": 1, "hint_penalty_s": [120, 60]}], "at least as much"),
        ([{"id": "a", "net": 1, "node": 1, "points": -1}], "negative"),
    ],
)
def test_bad_games(boxes, error):
    with pytest.raises(GameError, match=error):
        parse_game({"game": {"id": "g"}, "box": boxes})


# ------------------------------------------------------------------ the rules


def test_a_team_plays_through():
    run = make_run()
    assert play(run, RED, "vault", T0).verdict == Verdict.ACCEPTED
    assert play(run, RED, "garden", T0 + 10 * S).verdict == Verdict.ACCEPTED
    assert play(run, RED, "lab", T0 + 20 * S).verdict == Verdict.ACCEPTED
    r = play(run, RED, "tower", T0 + 30 * S, took=5 * S)
    assert r.verdict == Verdict.FINISHED
    assert (r.solved_required, r.required_total) == (4, 4)
    red = run.teams["red"]
    assert red.finished_at == T0 + 35 * S
    assert run.points(red) == 500
    assert run.time_ms(red) == 8 * S  # 1+1+1+5 s at boxes; walking between them is free
    assert run.state == "running"  # blue hasn't finished
    assert not run.sessions


def test_start_tells_the_team_before_it_plays():
    run = make_run()
    r = start(run, RED, "tower", T0)
    assert r.verdict == Verdict.LOCKED and r.needs == "vault"  # lab isn't open yet either
    play(run, RED, "vault", T0 + S)
    assert start(run, RED, "vault", T0 + 5 * S).verdict == Verdict.ALREADY_SOLVED
    play(run, RED, "lab", T0 + 6 * S)
    assert run.judge_start(RED, run.game.boxes["tower"], T0 + 9 * S).needs == "garden"
    assert not run.sessions


def test_win_needs_a_session():
    run = make_run()
    assert win(run, RED, "vault", T0 + S).verdict == Verdict.NO_SESSION
    start(run, BLUE, "vault", T0 + 2 * S)
    assert win(run, RED, "vault", T0 + 3 * S).verdict == Verdict.NO_SESSION  # blue's session
    assert "vault" not in run.teams["red"].solved


def test_one_team_at_a_box():
    run = make_run()
    start(run, RED, "vault", T0)
    r = start(run, BLUE, "vault", T0 + 1 * MIN)
    assert r.verdict == Verdict.BUSY and r.arg == 9 * 60  # seconds until red's session times out
    assert start(run, RED, "vault", T0 + 2 * MIN).verdict == Verdict.GO  # red again: continues
    assert run.sessions["vault"].start == T0
    win(run, RED, "vault", T0 + 3 * MIN)
    assert start(run, BLUE, "vault", T0 + 3 * MIN + S).verdict == Verdict.GO  # free now


def test_a_quiet_session_times_out_and_costs_the_timeout():
    run = make_run()
    start(run, RED, "vault", T0)
    wrong(run, RED, "vault", T0 + 2 * MIN)  # activity: the timeout runs from here
    assert start(run, BLUE, "vault", T0 + 11 * MIN).verdict == Verdict.BUSY
    assert start(run, BLUE, "vault", T0 + 12 * MIN).verdict == Verdict.GO
    red = run.teams["red"]
    assert red.box_time["vault"] == 12 * MIN  # start to last activity + the 10 min timeout
    assert run.time_ms(red) == 12 * MIN + 30 * S


def test_end_closes_the_session():
    run = make_run()
    start(run, RED, "vault", T0)
    run.apply({"kind": "end", "at": T0 + 90 * S, "box": "vault", "team": "red"})
    assert not run.sessions and run.teams["red"].box_time["vault"] == 90 * S
    play(run, RED, "vault", T0 + 10 * MIN, took=30 * S)
    assert run.teams["red"].box_time["vault"] == 120 * S  # both sessions count


def test_operator_closes_a_session():
    run = make_run()
    start(run, RED, "vault", T0)
    run.apply({"kind": "session_closed", "at": T0 + 5 * S, "box": "vault"})
    assert start(run, BLUE, "vault", T0 + 6 * S).verdict == Verdict.GO


def test_hints_cost_more_each_time_and_repeat_free():
    run = make_run()
    start(run, RED, "vault", T0)
    assert hint(run, RED, "vault", 2, T0 + S).verdict == Verdict.NO_MORE_HINTS  # 1 first
    assert hint(run, RED, "vault", 1, T0 + 2 * S).arg == 1
    assert hint(run, RED, "vault", 1, T0 + 3 * S).verdict == Verdict.HINT  # again: free
    assert hint(run, RED, "vault", 2, T0 + 4 * S).arg == 2
    r = hint(run, RED, "vault", 3, T0 + 5 * S)
    assert r.verdict == Verdict.NO_MORE_HINTS and r.arg == 2
    red = run.teams["red"]
    assert red.hints["vault"] == 2 and red.penalty_ms == 180 * S
    run.apply({"kind": "end", "at": T0 + 10 * S, "box": "vault", "team": "red"})
    assert start(run, RED, "vault", T0 + 20 * S).arg == 2  # go, with 2 hints already taken
    assert hint(run, RED, "cellar", 1, T0 + 30 * S).verdict == Verdict.NO_SESSION
    start(run, RED, "cellar", T0 + 31 * S)
    assert hint(run, RED, "cellar", 1, T0 + 32 * S).verdict == Verdict.NO_MORE_HINTS


def test_wrong_attempts_count_only_in_a_session():
    run = make_run()
    wrong(run, RED, "vault", T0)
    start(run, RED, "vault", T0 + S)
    wrong(run, RED, "vault", T0 + 2 * S)
    wrong(run, RED, "vault", T0 + 3 * S)
    red = run.teams["red"]
    assert red.wrong == {"vault": 2} and red.penalty_ms == 60 * S


def test_unknown_token_and_not_running():
    run = make_run()
    box = run.game.boxes["vault"]
    assert run.judge_start(b"\x99", box, T0 + 1).verdict == Verdict.UNKNOWN_TEAM
    assert run.judge_start(RED, box, T0 + 3600_000).verdict == Verdict.NOT_RUNNING  # deadline
    start(run, RED, "vault", T0 + S)
    run.apply({"kind": "run_ended", "at": T0 + 10 * S})
    assert run.teams["red"].box_time["vault"] == 9 * S  # closed when the run ended
    assert run.judge_win(RED, box, T0 + 11 * S).verdict == Verdict.NOT_RUNNING


def test_not_running_before_start():
    game = parse_game(GAME)
    run = Run(game, "r")
    run.apply({"kind": "team_entered", "at": 0, "team": "red", "tokens": [RED.hex()]})
    assert run.judge_start(RED, game.boxes["vault"], 5).verdict == Verdict.NOT_RUNNING


def test_optional_box_doesnt_count_toward_finishing():
    run = make_run()
    r = play(run, RED, "cellar", T0)
    assert r.verdict == Verdict.ACCEPTED and r.solved_required == 0
    assert run.points(run.teams["red"]) == 50


def test_run_ends_when_every_team_finishes():
    run = make_run(("solo", RED))
    for i, b in enumerate(["vault", "garden", "lab", "tower"]):
        play(run, RED, b, T0 + i * MIN)
    assert run.state == "ended" and run.ended_at == T0 + 3 * MIN + S
    assert run.winner() == "solo"


def test_tokens_are_unique_in_a_run():
    run = make_run()
    with pytest.raises(GameError, match="already issued"):
        run.apply({"kind": "team_entered", "at": 0, "team": "green", "tokens": [RED.hex()]})
    with pytest.raises(GameError, match="already in"):
        run.apply({"kind": "team_entered", "at": 0, "team": "red", "tokens": ["01"]})


def test_corrections():
    run = make_run()
    run.apply({"kind": "correction", "at": T0 + 1, "team": "red", "box": "vault", "action": "add"})
    assert play(run, RED, "lab", T0 + 2).verdict == Verdict.ACCEPTED
    run.apply({"kind": "correction", "at": T0 + 3 * S, "team": "red", "box": "lab", "action": "remove"})
    assert "lab" not in run.teams["red"].solved


def test_correction_can_unfinish():
    run = make_run()
    for i, b in enumerate(["vault", "garden", "lab", "tower"]):
        play(run, RED, b, T0 + i * MIN)
    run.apply({"kind": "correction", "at": T0 + 9 * MIN, "team": "red", "box": "tower", "action": "remove"})
    assert run.teams["red"].finished_at is None


# ------------------------------------------------------------------ standings


def test_standings_order():
    teams = [("a", b"\x0a"), ("b", b"\x0b"), ("c", b"\x0c"), ("d", b"\x0d"), ("e", b"\x0e")]
    run = make_run(*teams)
    tok = dict(teams)
    # a and b both finish. b finishes later on the wall clock, but spent less time at the boxes.
    for i, box in enumerate(["vault", "garden", "lab", "tower"]):
        play(run, tok["a"], box, T0 + i * MIN, took=20 * S)
        play(run, tok["b"], box, T0 + 30 * MIN + i * MIN, took=10 * S)
    # c, d and e haven't finished. e has the most points (cellar); c and d tie on points, d is faster.
    play(run, tok["c"], "vault", T0, took=40 * S)
    play(run, tok["d"], "vault", T0 + MIN, took=20 * S)
    play(run, tok["e"], "cellar", T0 + 2 * MIN)
    play(run, tok["e"], "vault", T0 + 3 * MIN, took=5 * MIN)
    s = run.standings()
    assert [x["team"] for x in s] == ["b", "a", "e", "d", "c"]
    assert (s[0]["points"], s[0]["time_ms"]) == (500, 40 * S)
    assert run.winner() == "b"


def test_penalties_decide_between_equal_teams():
    run = make_run(("a", b"\x0a"), ("b", b"\x0b"))
    start(run, b"\x0a", "vault", T0)
    hint(run, b"\x0a", "vault", 1, T0 + S)
    win(run, b"\x0a", "vault", T0 + 10 * S)
    start(run, b"\x0b", "garden", T0)
    wrong(run, b"\x0b", "garden", T0 + S)
    win(run, b"\x0b", "garden", T0 + 10 * S)
    s = run.standings()
    assert [x["team"] for x in s] == ["b", "a"]  # 10 s + 30 s beats 10 s + 60 s
    assert (s[0]["wrong"], s[1]["hints"]) == (1, 1)


def test_live_standings_count_open_sessions():
    run = make_run()
    start(run, RED, "vault", T0)
    assert run.time_ms(run.teams["red"]) == 0
    assert run.time_ms(run.teams["red"], now=T0 + 30 * S) == 30 * S
    assert run.time_ms(run.teams["red"], now=T0 + 60 * MIN) == 10 * MIN  # capped at the timeout


def test_no_winner_until_someone_finishes():
    run = make_run()
    play(run, RED, "vault", T0)
    assert run.winner() is None
