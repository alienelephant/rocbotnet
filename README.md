# roclink scavenger hunt

An IoT scavenger hunt played with [roclink](../roclink-rfc). Teams solve physical puzzle boxes; each box has a roclink node in it. When a team solves a box, the team taps its card on it and the box sends a `WIN`. This service judges the claim, records it, and sends the box its verdict. The first team to solve every required box wins.

- **[docs/GAME.md](docs/GAME.md)** is the game: vocabulary, the formal model, rules, flows, the messages on the wire, and how cheating is stopped.
- **`scavhunt/`** is the backend, a roclink hub service over MQTT.

## How it fits together

```
boxes ──roclink──► gateways ──► hub ──roclink/svc/scavhunt/up──► scavhunt serve ──► game.db (event log)
                                    ◄─roclink/svc/scavhunt/down──        │
                                                                         └──► http://…:8090 scoreboard
```

The hub verifies, decrypts and de-duplicates the boxes' frames, then delivers them to this service. The service never sees keys or radio frames, and the hub knows nothing about the game.

| File | What it does |
|---|---|
| `scavhunt/game.py` | The rules (GAME.md §3, §4): game definitions, runs, claims, standings. No I/O |
| `scavhunt/messages.py` | `WIN`, `ATTEMPT`, `VERDICT` and `CONTROL` payloads (GAME.md §6) |
| `scavhunt/store.py` | The append-only event log in SQLite. A run's state is its events replayed |
| `scavhunt/service.py` | The hub service: records in, verdicts out |
| `scavhunt/api.py` | Read-only HTTP: the scoreboard page, `/api/run`, `/api/standings`, `/api/events` |
| `scavhunt/__main__.py` | The operator's commands |

## Running a game

```bash
python -m venv .venv && .venv/bin/pip install -e .
cp game.example.toml game.toml        # your boxes' addresses, prerequisites, time limit
scavhunt check                         # validates it, shows which boxes open first

scavhunt serve &                       # the service and the scoreboard (http://127.0.0.1:8090)
scavhunt new-run                       # a run: becomes the current one
scavhunt team red  --name "Red"  --token 04:a1:b2:c3:d4:e5:f6
scavhunt team blue --name "Blue" --token text:BLUE42
scavhunt start
scavhunt status                        # standings in the terminal
scavhunt control tower reset           # tell a box to reset its puzzle
scavhunt solve blue lab --note "box was offline"   # an operator correction (and unsolve)
scavhunt end                           # or the time limit, or every team finishing
```

Operator commands write to the same event log, and the running service picks them up within a second, so you never restart it during a game.

## The hub's side

In the hub's config, register the boxes as one device type, always-listening so that verdicts are sent at once, and route them to this service:

```toml
[[device_type]]
name = "scavhunt-box"
class = "always_listening"

[[node]]
net = 0x42
nodes = ["0x0200-0x02FF"]
type = "scavhunt-box"

[[route]]
service = "scavhunt"
net = 0x42
types = ["scavhunt-box"]
```

Give the service a broker user named `scavhunt` that can read `roclink/svc/scavhunt/up` and `roclink/svc/scavhunt/down_status`, and write `roclink/svc/scavhunt/down`, as the hub's `deploy/mosquitto.acl` does for every service.

Boxes must send at security level `enc` (GAME.md rule 1). For a bench test with unsecured boxes, set `allow_insecure = true` under `[service]`.

## Tests

```bash
.venv/bin/pip install -e '.[dev]' && .venv/bin/pytest
```

They cover the rules, the payloads, the service with a fake hub, the scoreboard over HTTP, and the CLI. They don't use a real broker.

## License

MIT, see [LICENSE](LICENSE).
