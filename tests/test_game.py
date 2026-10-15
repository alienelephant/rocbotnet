import pytest

from scavhunt.game import GameError, Run, Verdict, parse_game

GAME = {
    "game": {"id": "g", "title": "Test", "time_limit_min": 60},
    "box": [
        {"id": "vault", "net": 0x42, "node": 0x201},
        {"id": "lab", "net": 0x42, "node": 0x202, "requires": ["vault"]},
        {"id": "garden", "net": 0x42, "node": 0x203},
        {"id": "tower", "net": 0x42, "node": 0x204, "requires": ["lab", "garden"]},
        {"id": "cellar", "net": 0x42, "node": 0x205, "required": False},
    ],
}
RED, BLUE = b"\x04\xa1", b"\x04\xb2"
T0 = 1_000_000


def make_run(*teams):
    game = parse_game(GAME)
    run = Run(game, "r1")
    for tid, tok in teams or (("red", RED), ("blue", BLUE)):
        run.apply({"kind": "team_entered", "at": 0, "team": tid, "tokens": [tok.hex()]})
    run.apply({"kind": "run_started", "at": T0})
    return run


def claim(run, token, box_id, at):
    r = run.judge(token, run.game.boxes[box_id], at)
    run.apply({"kind": "claim", "at": at, "box": box_id, "team": r.team, "verdict": int(r.verdict)})
    return r


# ---------------------------------------------------------------- the game file


def test_parse():
    g = parse_game(GAME)
    assert g.required == ["vault", "lab", "garden", "tower"]
    assert g.time_limit_s == 3600
    assert g.box_at(0x42, 0x204).id == "tower"
    assert g.box_at(0x43, 0x204) is None


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
    ],
)
def test_bad_games(boxes, error):
    with pytest.raises(GameError, match=error):
        parse_game({"game": {"id": "g"}, "box": boxes})


# ------------------------------------------------------------------ the rules


def test_a_team_plays_through():
    run = make_run()
    assert claim(run, RED, "vault", T0 + 1).verdict == Verdict.ACCEPTED
    assert claim(run, RED, "garden", T0 + 2).verdict == Verdict.ACCEPTED
    assert claim(run, RED, "lab", T0 + 3).verdict == Verdict.ACCEPTED
    r = claim(run, RED, "tower", T0 + 4)
    assert r.verdict == Verdict.FINISHED
    assert (r.solved_required, r.required_total) == (4, 4)
    assert run.teams["red"].finished_at == T0 + 4
    assert run.state == "running"  # blue hasn't finished


def test_locked_names_an_open_prerequisite():
    run = make_run()
    r = claim(run, RED, "tower", T0 + 1)
    assert r.verdict == Verdict.LOCKED
    assert r.needs == "vault"  # lab isn't open yet either, so point at vault
    claim(run, RED, "vault", T0 + 2)
    claim(run, RED, "lab", T0 + 3)
    assert run.judge(RED, run.game.boxes["tower"], T0 + 4).needs == "garden"
    assert "tower" not in run.teams["red"].solved


def test_already_solved_and_other_teams():
    run = make_run()
    claim(run, RED, "vault", T0 + 1)
    assert claim(run, RED, "vault", T0 + 5).verdict == Verdict.ALREADY_SOLVED
    assert run.teams["red"].solved["vault"] == T0 + 1  # the first solve's time stands
    assert claim(run, BLUE, "vault", T0 + 6).verdict == Verdict.ACCEPTED  # shared boxes


def test_unknown_token_and_not_running():
    run = make_run()
    assert run.judge(b"\x99", run.game.boxes["vault"], T0 + 1).verdict == Verdict.UNKNOWN_TEAM
    assert run.judge(RED, run.game.boxes["vault"], T0 + 3600_000).verdict == Verdict.NOT_RUNNING
    run.apply({"kind": "run_ended", "at": T0 + 10})
    assert run.judge(RED, run.game.boxes["vault"], T0 + 11).verdict == Verdict.NOT_RUNNING


def test_not_running_before_start():
    game = parse_game(GAME)
    run = Run(game, "r")
    run.apply({"kind": "team_entered", "at": 0, "team": "red", "tokens": [RED.hex()]})
    assert run.judge(RED, game.boxes["vault"], 5).verdict == Verdict.NOT_RUNNING


def test_optional_box_doesnt_count_toward_finishing():
    run = make_run()
    r = claim(run, RED, "cellar", T0 + 1)
    assert r.verdict == Verdict.ACCEPTED and r.solved_required == 0


def test_run_ends_when_every_team_finishes():
    run = make_run(("solo", RED))
    for i, b in enumerate(["vault", "garden", "lab", "tower"]):
        claim(run, RED, b, T0 + i)
    assert run.state == "ended" and run.ended_at == T0 + 3
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
    assert claim(run, RED, "lab", T0 + 2).verdict == Verdict.ACCEPTED
    run.apply({"kind": "correction", "at": T0 + 3, "team": "red", "box": "lab", "action": "remove"})
    assert "lab" not in run.teams["red"].solved


def test_correction_can_unfinish():
    run = make_run()
    for i, b in enumerate(["vault", "garden", "lab", "tower"]):
        claim(run, RED, b, T0 + i)
    run.apply({"kind": "correction", "at": T0 + 9, "team": "red", "box": "tower", "action": "remove"})
    assert run.teams["red"].finished_at is None


# ------------------------------------------------------------------ standings


def test_standings_order():
    teams = [("a", b"\x0a"), ("b", b"\x0b"), ("c", b"\x0c"), ("d", b"\x0d"), ("e", b"\x0e")]
    run = make_run(*teams)
    tok = dict(teams)
    # b finishes first, a second
    for i, box in enumerate(["vault", "garden", "lab", "tower"]):
        claim(run, tok["b"], box, T0 + 10 + i)
        claim(run, tok["a"], box, T0 + 20 + i)
    # c and d each solve 2 required; d got there first. e solves 2 too, later, plus cellar.
    claim(run, tok["c"], "vault", T0 + 30)
    claim(run, tok["c"], "garden", T0 + 35)
    claim(run, tok["d"], "vault", T0 + 31)
    claim(run, tok["d"], "garden", T0 + 32)
    claim(run, tok["e"], "vault", T0 + 33)
    claim(run, tok["e"], "garden", T0 + 35)
    claim(run, tok["e"], "cellar", T0 + 36)
    order = [s["team"] for s in run.standings()]
    # c and e tie on last required... e's last solve (cellar) is later, so c is ahead
    assert order == ["b", "a", "d", "c", "e"]
    assert run.winner() == "b"


def test_optional_breaks_a_tie():
    run = make_run(("a", b"\x0a"), ("b", b"\x0b"))
    claim(run, b"\x0a", "vault", T0 + 5)
    claim(run, b"\x0b", "cellar", T0 + 1)
    claim(run, b"\x0b", "vault", T0 + 5)
    assert [s["team"] for s in run.standings()] == ["b", "a"]


def test_no_winner_until_someone_finishes():
    run = make_run()
    claim(run, RED, "vault", T0 + 1)
    assert run.winner() is None
