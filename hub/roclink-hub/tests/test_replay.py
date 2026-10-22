from roclink_hub.replay import DUPLICATE, NEW, REPLAY, WINDOW, ReplayGuard, classify


def run(seq):
    highest, seen, out = None, 0, []
    for f in seq:
        verdict, highest, seen = classify(highest, seen, f)
        out.append(verdict)
    return out


def test_new_duplicate_replay():
    assert run([5, 5, 6, 4, 4, 3]) == [NEW, DUPLICATE, NEW, NEW, DUPLICATE, NEW]


def test_out_of_order_within_the_window_is_new_once():
    # Two gateways forward frames 10 and 9 in the wrong order: both are new.
    assert run([10, 9, 10, 9]) == [NEW, NEW, DUPLICATE, DUPLICATE]


def test_below_the_window_is_a_replay():
    assert run([100, 100 - WINDOW + 1, 100 - WINDOW]) == [NEW, NEW, REPLAY]


def test_a_big_jump_clears_the_window():
    assert run([1, 1000, 999, 1]) == [NEW, NEW, NEW, REPLAY]


def test_the_window_survives_a_restart_and_keeps_key_generations_apart(tmp_path):
    path = tmp_path / "w.db"
    g = ReplayGuard(path)
    assert g.check(0x42, 0x1234, 0, 70) == NEW
    assert g.check(0x42, 0x1234, 0, 70) == DUPLICATE
    g.close()
    g = ReplayGuard(path)  # restarted
    assert g.check(0x42, 0x1234, 0, 70) == DUPLICATE
    assert g.check(0x42, 0x1234, 0, 5) == REPLAY
    assert g.check(0x42, 0x1234, 0, 69) == NEW
    assert g.check(0x42, 0x1234, 1, 5) == NEW  # a new key generation restarts counters
    assert g.check(0x42, 0x1235, 0, 5) == NEW  # another node
    # The top bit of the 64-bit bitmap round-trips through SQLite's signed integers.
    assert g.check(0x42, 0x1234, 0, 70 - 63) == NEW
    g.close()
    g = ReplayGuard(path)
    assert g.check(0x42, 0x1234, 0, 70 - 63) == DUPLICATE
