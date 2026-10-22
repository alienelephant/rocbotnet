"""The supervisor: starts the processes, watches them, and stops them in order.

                ┌──(queue)──> writer ──> hub.db    <── api (read-only)
    ingest ─────┤
                └──(queue)──> public writer ──> public.db <── public api (read-only)
    router ────> roclink/svc/<service>/up, roclink/pub/up
    downlink ──> roclink/out/<net>/<gateway>/down   (requests from roclink/svc/<service>/down)

Start: the writers first. Each creates or migrates its schema, then signals
ready. Only then do ingest, the router and the APIs start, so readers never
see a database without its schema.

Stop (SIGTERM or SIGINT): ingest and the router first, so no new messages
arrive. Then STOP goes on each writer's queue behind the last message, and
each writer commits everything before it and exits. The APIs go last.

If any process dies, the supervisor stops the rest and exits non-zero, and
systemd restarts the whole service. Restarting one child in place would
leave the others guessing about the queues' state.
"""

import logging
import multiprocessing
import multiprocessing.connection
import queue as queue_module
import signal

from . import api, downlink, ingest, router, writer
from .process import STOP

log = logging.getLogger(__name__)

PRODUCER_STOP_TIMEOUT_S = 10
WRITER_STOP_TIMEOUT_S = 30
API_STOP_TIMEOUT_S = 5


def run(cfg) -> int:
    # "spawn" starts each child from a fresh interpreter: nothing is shared
    # but the arguments, as with separate programs.
    ctx = multiprocessing.get_context("spawn")
    stop = ctx.Event()

    def request_stop(signum, frame):
        log.info("%s received, stopping", signal.Signals(signum).name)
        stop.set()

    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)

    # Writers: (process, queue, ready).
    private_queue = ctx.Queue(maxsize=cfg.writer.queue_size)
    public_queue = ctx.Queue(maxsize=cfg.writer.queue_size) if cfg.public.enabled else None
    writers = [_writer(ctx, "writer", cfg, cfg.db_path, None, private_queue)]
    if public_queue is not None:
        retention_ms = int(cfg.public.retention_h * 3_600_000)
        writers.append(
            _writer(ctx, "public-writer", cfg, cfg.public.db_path, retention_ms, public_queue)
        )
    for p, _, _ in writers:
        p.start()
    if not _wait_ready(writers, stop):
        _stop_writers(writers)
        return 1

    producers, readers = [], []
    if not stop.is_set():
        producers.append(
            ctx.Process(
                target=ingest.run, args=(cfg, private_queue, public_queue, stop), name="ingest"
            )
        )
        if cfg.router.enabled:
            producers.append(ctx.Process(target=router.run, args=(cfg, stop), name="router"))
        if cfg.downlink.enabled:
            producers.append(ctx.Process(target=downlink.run, args=(cfg, stop), name="downlink"))
        if cfg.api.enabled:
            readers.append(
                ctx.Process(target=api.run, args=(cfg, cfg.db_path, cfg.api, stop), name="api")
            )
        if cfg.public.enabled and cfg.public.api.enabled:
            readers.append(
                ctx.Process(
                    target=api.run,
                    args=(cfg, cfg.public.db_path, cfg.public.api, stop),
                    name="public-api",
                )
            )
        for p in producers + readers:
            p.start()

    children = [p for p, _, _ in writers] + producers + readers
    failed = False
    while not stop.is_set():
        multiprocessing.connection.wait([p.sentinel for p in children], timeout=1.0)
        dead = [p for p in children if not p.is_alive()]
        # Children exit on their own only after stop is set. Checking stop
        # *after* collecting the dead tells a requested stop from a crash.
        if dead and not stop.is_set():
            for p in dead:
                log.error("%s exited unexpectedly (exit code %s)", p.name, p.exitcode)
            failed = True
            stop.set()

    stop.set()
    for p in producers:
        _join(p, PRODUCER_STOP_TIMEOUT_S)
    if not _stop_writers(writers):
        failed = True
    for p in readers:
        _join(p, API_STOP_TIMEOUT_S)

    log.info("stopped%s", " after a failure" if failed else "")
    return 1 if failed else 0


def _writer(ctx, name, cfg, db_path, retention_ms, queue):
    ready = ctx.Event()
    p = ctx.Process(
        target=writer.run, args=(cfg, db_path, retention_ms, queue, ready), name=name
    )
    return p, queue, ready


def _wait_ready(writers, stop) -> bool:
    """Wait until every writer has its schema ready. False if one died first."""
    for p, _, ready in writers:
        while not ready.wait(0.5):
            if not p.is_alive():
                log.error("%s failed to start (exit code %s)", p.name, p.exitcode)
                return False
            if stop.is_set():
                return True
    return True


def _stop_writers(writers) -> bool:
    """Put STOP behind each writer's last message and wait for it to commit.
    Call only once nothing else puts on the queues. True if all exited cleanly."""
    for p, queue, _ in writers:
        if p.is_alive():
            try:
                queue.put(STOP, timeout=5)
            except queue_module.Full:
                log.error("%s's queue is full and it isn't draining it", p.name)
    ok = True
    for p, queue, _ in writers:
        _join(p, WRITER_STOP_TIMEOUT_S)
        # Don't wait to flush the queue to a writer that's gone.
        queue.cancel_join_thread()
        ok = ok and p.exitcode == 0
    return ok


def _join(p, timeout: float) -> None:
    p.join(timeout)
    if p.is_alive():
        # Children ignore SIGTERM (the supervisor sequences shutdown), so kill.
        log.error("%s didn't stop within %ss, killing it", p.name, timeout)
        p.kill()
        p.join()
