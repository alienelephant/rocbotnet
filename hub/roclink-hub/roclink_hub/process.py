"""Helpers shared by the supervisor and its child processes."""

import logging
import os
import signal
import time
from collections.abc import Callable

# Put on the queue after the last message. The writer stops when it gets it.
STOP = None


def now_ms() -> int:
    return time.time_ns() // 1_000_000


def setup_logging(level: str) -> None:
    # No timestamps: journald adds them.
    logging.basicConfig(level=level, format="%(processName)s %(levelname)s %(name)s: %(message)s")


def child_init(level: str) -> Callable[[], bool]:
    """Common start for every child process.

    Children run with the "spawn" start method, so they share nothing with
    the supervisor but their arguments; logging is set up again here.
    SIGINT and SIGTERM are ignored: the supervisor decides when and in what
    order children stop. Returns a function that says whether the supervisor
    is gone, so an orphaned child can exit on its own.
    """
    setup_logging(level)
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    parent = os.getppid()
    return lambda: os.getppid() != parent
