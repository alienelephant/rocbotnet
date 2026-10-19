# roclink-hub

A demo hub: one implementation of the system behind the gateways ("hub / server software" in roclink's use cases). It isn't part of the wire protocol, and it doesn't define what a hub must be. It speaks the draft gateway interface ([docs/GATEWAY_INTERFACE.md](docs/GATEWAY_INTERFACE.md)), and does the few things the wire protocol requires of any hub ([PROTOCOL.md §9.1](../roclink-rfc/docs/PROTOCOL.md#91-what-the-hub-must-do)). Everything else here is one way to do it, free to change.

It drains roclink uplinks from MQTT and:

- stores them in SQLite for other processes to read;
- delivers them to the services they're routed to;
- refuses secured frames replayed from one gateway to another (it's a security control: [PROTOCOL.md §9.1](../roclink-rfc/docs/PROTOCOL.md#91-what-the-hub-must-do));
- queues services' downlink, numbers it per node, and sends it through the right gateway;
- keeps a public net, separate from everything else, for developers testing devices;
- counts the frames gateways drop, and alerts operators to likely attacks.

It's written for a Raspberry Pi 3B+ and kept plain so that it can be ported to Rust later.

**Status:** roclink 0.3. No gateway in this repo speaks MQTT yet: the downlink topics (GATEWAY_INTERFACE.md §5) are implemented on the hub's side, and a gateway has to implement its side.

## Architecture

```
                         ┌──────────── roclink-hub (one systemd service) ──────────────────────────┐
 gateways                │            ┌─(queue)─► writer ───────► hub.db     ◄── api (read-only)   │
 ──MQTT──►               │  ingest ───┤                            kept for good                   │
 roclink/in/+/+/up       │  monitor   └─(queue)─► public-writer ► public.db  ◄── public-api (r/o)  │
 roclink/in/+/+/rejected │     └────► roclink/ops/rejected        24 h       (operators only)     │
                         │  router ──► roclink/svc/<service>/up   (each service: its own ACL)      │
                         │         ──► roclink/pub/up              (anyone can read)               │
 roclink/svc/+/down ────►│  downlink ──► roclink/out/<net>/<gw>/down   (one gateway per frame)     │
                         │  supervisor: start order, watch, stop order                             │
                         └─────────────────────────────────────────────────────────────────────────┘
```

| Process | Does | Never does |
|---|---|---|
| `supervisor` | Starts the writers first, then everything else once the schemas are ready. Stops them in order. If any process dies, stops the rest and exits 1 so systemd restarts everything. | Touch MQTT or a DB |
| `ingest` | Subscribes to the gateway topics (QoS 1, persistent session). Parses each message, checks secured uplinks against its anti-replay window, decodes the payload by device type and schema, and queues it for `hub.db` or, if on a public net, `public.db`. Counts rejected frames (the monitor), and publishes alerts and summaries. | Open a DB (it keeps its window in its own state file) |
| `writer` | The only writer of `hub.db`. Takes batches off its queue (up to 200 messages or 0.5 s), writes each batch in one transaction, and removes duplicates | Talk to MQTT |
| `public-writer` | The same, for `public.db`, which it also prunes to `retention_h` | Talk to MQTT |
| `router` | Its own MQTT session on the gateways' uplink topics. Checks each secured uplink against its anti-replay window, then delivers it to the services whose routes match, or to the public topic | Open a DB (it keeps its window in its own state file) |
| `downlink` | Its own MQTT session on services' requests, gateways' uplinks and gateways' downlink statuses. Queues and numbers downlink per node, picks the gateway, re-offers, and tells the service the outcome | Touch `hub.db` (it keeps its queue in `downlink.db`) |
| `api`, `public-api` | Read-only JSON over HTTP, each over one database | Write |

**Why one writer per database?** SQLite allows one writer at a time anyway. With a single writing process per file:
- there is no `database is locked` contention and no retry logic;
- duplicate checks are plain check-then-insert, with nothing to race them;
- batching cuts commits and SD card wear;
- other processes can only read.

The public net gets its own file, writer and queue, so a flood of public traffic can't slow private storage or bloat `hub.db`.

**Why WAL?** Readers never block the writer and the writer never blocks readers. Each read transaction sees a consistent snapshot, so a batch shows up all at once. `synchronous=NORMAL` survives a crash of the process. A power cut can lose the last few commits but never corrupts the file.

**Why is the router separate from ingest?** Each has its own MQTT session, so the broker keeps a separate backlog for each. A slow writer can't hold up delivery to services, and a backlog of service deliveries can't hold up storage.

**Stopping.** On SIGTERM, ingest and the router disconnect first. Then a STOP marker goes on each writer's queue behind its last message, so each writer commits everything queued before it exits. Children ignore signals themselves: the supervisor decides the order.

## Topics

```
roclink/in/<net>/<gateway>/up     gateways → hub         only gateways write; only the hub reads
roclink/in/<net>/<gateway>/rejected  gateways → hub      what gateways dropped, and why
roclink/ops/rejected              hub → operators        rejection alerts and summaries
roclink/svc/<service>/up          hub → one service      that service's broker user reads it
roclink/svc/<service>/down        service → hub          downlink requests (below)
roclink/svc/<service>/down_status hub → one service      what became of each request
roclink/pub/up                    hub → anyone           anonymous read; only the hub writes
roclink/out/<net>/<gateway>/down  hub → one gateway      a downlink to send (GATEWAY_INTERFACE.md §5)
roclink/in/<net>/<gateway>/down_status  gateway → hub    acked, sent or expired
```

Services should subscribe to `roclink/svc/<service>/#`, not `.../up`, so that finer levels (`.../up/<net>/<node>/<schema>`) can be added later without breaking them.

Deliveries use the forward record format below, plus `hub_time` and the decoder's output (`decoder`, `decoded`). Delivery is at-least-once: the router drops copies from several gateways that arrive within `dedupe_window_s`, but a restart or a QoS 1 redelivery can still repeat one. A service should ignore repeats of `(net, src, key_gen, fcnt)`, or of `(net, src, seq)` for an unsecured uplink.

A secured uplink is checked against the router's persistent anti-replay window first ([The hub as a security control](#the-hub-as-a-security-control)): a copy of the same frame from another gateway is delivered once, ever, and a frame replayed to a gateway that never heard it isn't delivered at all.

## Services

A service is a `[[route]]` in the config plus a broker user with the same name:

```toml
[[route]]
service = "scavhunt"                   # topic level and broker username
net = 0x42
schemas = [20, 21]                       # omit: every schema
nodes = ["0x0100-0x01FF", "0x1234"]    # omit: every node
# allow_insecure = false               # true: also deliver uplinks with security "none"
```

- **Matching:** routes match on the envelope's net, schema and node. Gateways verify secured frames before publishing, so these fields can be trusted.
- **Fan-out:** an uplink goes to every service with a matching route, once each.
- **Storage:** everything on a private net is stored in `hub.db`, whether it was routed or not.
- **Security:** by default a route delivers only `auth` or `enc` uplinks. An unsecured uplink on a private net is stored but not delivered unless the route sets `allow_insecure`.

Adding a service:
1. Add its `[[route]]` and restart the hub.
2. Run `mosquitto_passwd /etc/mosquitto/passwd scavhunt` and reload Mosquitto.

The pattern lines in [deploy/mosquitto.acl](deploy/mosquitto.acl) give each broker user `roclink/svc/<its name>/#` and nothing else, so the ACL doesn't change per service. Names starting with `roclink` are reserved for the hub's and gateways' own users.

## The public net

For developers testing a device with no keys and no account. It's a separate **net id** (for example `0xF0`), not a topic or a schema, so roclink's own receiver policy ([PROTOCOL.md §10.1](../roclink-rfc/docs/PROTOCOL.md#101-levels-and-policy)) keeps it apart. In the protocol's terms it is an *open net* ([PROTOCOL.md Annex B](../roclink-rfc/docs/PROTOCOL.md#annex-b-unsecured-operation)): no secured nodes, so it is the one kind of net allowed to use `allow-none` network-wide.

- **Gateway policy:** gateways use `allow-none` on the public net and `require-enc` on every other net. On a net with secured nodes, `allow-none` can only be set per node. An unsecured device works on the public net, but the same device set to a private net id is dropped at the gateway.
- **Never routed to a service:** the config refuses a `[[route]]` on a public net. Everything on it is visible to anyone, and anyone with a radio can claim any node id.
- **Stored apart:** in `public.db`, deleted after `retention_h` (24 h). It's pruned every minute in short transactions.
- **Rate-limited** in ingest and the router: `max_per_minute` in total and `max_per_node_per_minute` per node. The total matters most, because node ids can be forged. Excess messages are dropped and counted in the log.
- **Published** to `roclink/pub/up`, which anonymous clients may read.
- **Served** by `public-api`, a separate process that is only given `public.db`.

Only gateways can reach the public net: developers send radio frames, never MQTT. For the public web page:
- put a reverse proxy in front of `public-api`, or bridge `roclink/pub/#` to a broker off the Pi (Mosquitto `bridge`, `topic roclink/pub/# out`), rather than exposing the Pi itself;
- render payloads as hex or escaped text, never as HTML, because anyone can choose them.

## Rejected frames and alerts

Gateways report every frame they drop to `roclink/in/<net>/<gateway>/rejected` ([GATEWAY_INTERFACE.md §4](docs/GATEWAY_INTERFACE.md#4-drop-reports)), with a reason such as:
- `auth`: a forged frame;
- `replay`: a recorded frame sent again;
- `conflict`: a cloned node;
- `revoked`: a stolen device;
- `policy`: a downgrade attempt;
- `peers_full`: the gateway's secured peer store is full, usually a flood of invented addresses under open admission.

The hub adds its own drops:
- `replay`: a secured uplink below its node's anti-replay window, most likely recorded near one gateway and replayed to another;
- `malformed_record` and `malformed_report`: only gateways can publish on `roclink/in`, so either a gateway bug or a stolen gateway credential;
- `rate_limited` and `queue_full`: public-net uplinks it couldn't take.

Ingest's monitor ([monitor.py](roclink_hub/monitor.py)) counts these per window (`window_s`, 60 s) by source, gateway, net, claimed src and reason, and rates each summary:

| Severity | Reasons |
|---|---|
| `critical` | `conflict` (verified: a second device holds the node's key, or its counter was reset) |
| `high` | `revoked`, `malformed_record`, `malformed_report`; `burst` (20) or more `auth`, `replay`, `policy` or `peers_full` in one summary |
| `warn` | single `auth`, `replay`, `policy`, `peers_full`; `key`, `not_enrolled`, `epoch`, `queue_full`; reasons it doesn't know |
| `info` | `malformed`, `net`, `addr`, `relayed`, `unexpected`, `rate_limited`: normal on shared bands |

- **Alerts:** the moment a summary first reaches `high` or `critical`, ingest logs `ALERT` and publishes `{"kind": "alert", ...}` to `roclink/ops/rejected`. This happens once per summary per window.
- **Summaries:** when the window closes, every summary goes to the same topic (`{"kind": "rejections", ...}`) and into `hub.db`'s `rejections` table.
- **Querying:** `GET /api/rejections?severity=high&since=<unix ms>` lists them, and `/api/health` totals the last hour by severity.

**Floods stay bounded.** A forged frame costs an attacker one transmission. If each became a broker message and a database row, a flood would spread into the broker and onto the SD card. Instead:
- the hub keeps a claimed src only for nodes it has seen send verified uplinks, plus `conflict` and `revoked`, which can't be invented;
- every other claim counts as "unknown", so 10,000 invented addresses make one row per window;
- past `max_keys` summaries in a window, events are counted by reason alone.

Individual gateway reports aren't stored, only the summaries, each with the first sample frame and the RSSI range. The RSSI range is often the best evidence: a node usually received at −95 dBm whose frames fail `auth` at −40 dBm means a transmitter nearby.

**Everything but the reason is attacker-controlled.** It came from a frame the gateway refused. `claimed_src` is only a claim, and the sample is whatever was sent: escape both before showing them anywhere.

Only the `roclink-ops` broker user may read `roclink/ops/#`. The feed tells an attacker which nodes exist and what was noticed.

## What it stores

Both databases have the same schema:

| Table | One row per |
|---|---|
| `messages` | MQTT message, verbatim, with `status` `stored`, `duplicate` or `rejected` (with `error`). Nothing reaching a writer is silently dropped. |
| `uplinks` | Distinct uplink: envelope, payload, and the decoded JSON (or `decode_error`) |
| `receptions` | Gateway that forwarded an uplink: gateway id and time, transport, RSSI and SNR. Several gateways can hear one uplink. |
| `readings` | Numeric field of a decoded payload (`metric`, `value`), indexed for time series |
| `nodes` | Node: first and last seen, last schema, uplink count |
| `rejections` | Rejection summary: window, source, gateway, net, claimed src, reason, severity, count, RSSI range, a sample (`hub.db` only) |

Times are unix ms (UTC). Addresses are integers. The schema is versioned with `PRAGMA user_version` (see `db.MIGRATIONS`). Messages that can't be parsed go to `hub.db`, since a gateway sent them.

**Duplicates** ([PROTOCOL.md §10.5](../roclink-rfc/docs/PROTOCOL.md#105-frame-counters) and [PROTOCOL.md Annex B](../roclink-rfc/docs/PROTOCOL.md#annex-b-unsecured-operation)):
- **Secured:** the same `(net, node, key_gen, fcnt)` is the same frame, whenever it arrives. A unique index enforces this as well. A replay of an older frame never gets this far: ingest's anti-replay window drops it first.
- **Unsecured:** the same `seq`, `schema` and payload within `dedupe_window_s` (60 s).

A duplicate adds a `receptions` row, not a new uplink.

## Input: the gateway forward record

The format is drafted in [docs/GATEWAY_INTERFACE.md](docs/GATEWAY_INTERFACE.md), a separate draft RFC; whether it becomes normative is its open question 1. Gateways publish to `roclink/in/<net>/<gateway>/up` with hex ids, for example `roclink/in/42/ff01/up`. The MQTT payload is either:

**A JSON forward record (preferred).** The gateway has already checked the envelope and, for secured frames, verified and decrypted it:

```json
{"v": 1, "gateway": "0xFF01", "time": 1791460800123,
 "net": 66, "src": "0x1234", "dst": 0, "seq": 7, "schema": 1, "flags": 3,
 "sec": "enc", "fcnt": 1, "key_gen": 0,
 "payload": "29091810cd8b",
 "radio": {"transport": "lora", "rssi": -87, "snr": 7.5}}
```

- **Required:** `net`, `src`, `seq`, `schema` and `payload` (hex). `fcnt` and `key_gen` are also required when `sec` is `auth` or `enc`.
- **Ids:** a JSON number or a `"0x…"` string.
- **`gateway`:** defaults to the one in the topic.
- **`time`:** the gateway's receive time, in unix ms.

**A raw unsecured roclink frame.** This is for bench bridges that only move bytes, such as `tools/udp_bridge.py`. The hub decodes the envelope itself (Annex A). It rejects secured raw frames, because it holds no keys to verify them.

## Payload decoders

Schemas belong to device types ([PROTOCOL.md §5](../roclink-rfc/docs/PROTOCOL.md#5-message-schemas)): each maker's firmware owns its own schemas 1–255, so the hub decodes by `(device type, schema)`. The config says which nodes are which type, and nodes that aren't registered fall back to a deployment-wide `[schemas]` table:

```toml
[[device_type]]
name = "room-sensor"           # examples/c
class = "sleepy"               # or "always_listening": how its downlink is sent
schemas = { 1 = "env", 2 = "event" }

[[node]]
net = 0x42
nodes = ["0x1234", "0x0100-0x01FF"]
type = "room-sensor"

[schemas]                        # nodes with no device type
1 = "text"
```

The decoders here: `env` (int16 temp_cC, uint16 rh_cPct, uint16 batt_mV → temp_c, rh_pct, batt_v), `event` (uint8 event, uint32 count) and `text` (UTF-8). Delivered records carry the node's `device_type`, and a `[[route]]` can match on `types`.

To add a format, write a function in `roclink_hub/decoders.py` that takes `bytes`, returns a flat `dict`, and raises `ValueError` on bad input. Then add it to `DECODERS`. Every number in the returned dict also becomes a `readings` row. A payload that fails to decode is still stored and delivered, with `decode_error` set.

## Reading a database from another process

Open it read-only and keep transactions short (a long-open read stops WAL checkpoints and the `-wal` file grows):

```python
from roclink_hub import db
conn = db.connect_reader("/var/lib/roclink-hub/hub.db")   # mode=ro + query_only
```

Or use the shell: `sqlite3 "file:/var/lib/roclink-hub/hub.db?mode=ro"`. Readers need the hub to be running, or the `-wal` and `-shm` files to exist. A reader running as another user must be in the `roclink` group (see the unit file).

The APIs: `api` serves `hub.db` on localhost:8080, and `public-api` serves `public.db` on localhost:8081. Neither has auth.

```
GET /api/health                                       schema, last message, last hour by status and severity
GET /api/nodes
GET /api/uplinks?node=0x1234&schema=1&limit=50&before=<id>
GET /api/readings?node=0x1234&metric=temp_c&since=<unix ms>&limit=1000   -> [[t, value], ...]
```

## Run it locally

```bash
cd hub
python3 -m venv .venv && .venv/bin/pip install -e '.[dev]'
.venv/bin/pytest
cp config.example.toml dev.toml     # set the db paths to dev.db and dev-public.db
.venv/bin/roclink-hub --config dev.toml
```

To test with the Arduino Wi-Fi sender (`examples/arduino/wifi_udp_sender`), run a broker and the bench bridge on the same LAN. The sketch sends unsecured text on schema 1 on the open test net `0xF0`, which the example config treats as the public net: its uplinks go to `public.db`, served on port 8081.

```bash
.venv/bin/python tools/udp_bridge.py --broker localhost
curl -s localhost:8081/api/uplinks?limit=5
```

To play gateway by hand, and watch what a service receives:

```bash
mosquitto_sub -v -t 'roclink/svc/#' -t 'roclink/pub/#' -t 'roclink/ops/#' &
mosquitto_pub -q 1 -t roclink/in/42/ff01/up -m \
  '{"net":66,"src":"0x0150","seq":1,"schema":20,"sec":"enc","fcnt":1,"key_gen":0,"payload":"6869"}'
mosquitto_pub -q 1 -t roclink/in/42/ff01/rejected -m \
  '{"kind":"rejected","reason":"conflict","net":66,"src":"0x0150","fcnt":1}'   # an immediate alert
```

## Deploy on the Pi (Raspberry Pi OS Bookworm, Python 3.11)

```bash
sudo apt install python3-venv mosquitto mosquitto-clients
sudo useradd --system --home /var/lib/roclink-hub --shell /usr/sbin/nologin roclink
sudo python3 -m venv /opt/roclink-hub/venv
sudo /opt/roclink-hub/venv/bin/pip install /path/to/roclink-hub
sudo install -D -m 0640 -g roclink config.example.toml /etc/roclink-hub/config.toml
sudo install -m 0644 deploy/roclink-hub.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now roclink-hub
journalctl -u roclink-hub -f
```

**Mosquitto.** Install [deploy/mosquitto.acl](deploy/mosquitto.acl) as `/etc/mosquitto/roclink.acl` and add the settings at its top to `/etc/mosquitto/conf.d/roclink.conf`. Then create the broker users: `roclink-gw` (gateways), `roclink-hub` (set it as `[mqtt] username` and `password`), `roclink-ops` (whoever watches alerts), and one per service. `max_queued_messages` in those settings matters: Mosquitto's default keeps only 1000 messages per offline session.

Gateways must publish at QoS 1 for the broker to queue messages for the hub. [SECURITY.md](../roclink-rfc/docs/SECURITY.md) calls for TLS on the gateway → broker link. Service topics carry decrypted payloads, so use TLS for every service that isn't on the Pi itself. In the hub, set `tls` and `ca_file` in `[mqtt]`.

On x86-64, the first version's five processes used about 120 MB RSS in total. The router, public writer and public API each add another 20–25 MB. That's well within the 3B+'s 1 GB.

## Known limits

- **At-most-once between broker and DB.** paho acknowledges a QoS 1 message once it's on the in-memory queue, not once it's committed. A SIGKILL or power cut loses whatever is still queued (normally under a second's worth). A clean stop loses nothing. The fix is paho's `manual_ack`, with the writer reporting committed message ids back to ingest. The router has the same gap: it acknowledges once it has handed the delivery to paho.
- **Untested here: catch-up after a restart, and the ACL.** The broker used in development (amqtt) doesn't queue for offline sessions and doesn't use Mosquitto ACL files. On the Pi, check both:
  - **catch-up:** stop the hub, publish, start it, and look for the message;
  - **ACL:** a service user can read only its own subtree, and anonymous clients can read only `roclink/pub`.
- **`hub.db` grows forever,** by design. `messages` keeps every raw message for audit and re-decoding. At ~50 nodes reporting every 5 minutes, that's a few MB a day. `public.db` stays about one retention period's size: SQLite reuses freed pages rather than shrinking the file.
- **The public rate limits are per process.** Ingest and the router each count separately, so under a flood they may let through slightly different messages.
- **The routing table is read at start.** Changing a route means restarting the hub. Deliveries wait in the broker meanwhile, so nothing is lost.
- **No re-decode command.** If a decoder changes, past uplinks keep their old `decoded` value. The raw payloads are there to re-run decoders later, as a writer-side job.
- **Gateways must publish their reports.** `roclink-gw` (the roclink-gateway repository) publishes each drop to `roclink/in/<net>/<gateway>/rejected` with a rate bound, as GATEWAY_INTERFACE.md §4 asks. The Arduino receiver examples only print theirs.
- **Alerts go to MQTT and the log only.** Anything else (email, push) needs a subscriber to `roclink/ops/rejected`.
- **Monitor state is in memory.** A restart closes the current window early, after storing what it has, and forgets which nodes are known until they send again.
- **No gateway here speaks MQTT.** The downlink topics work on the hub's side; a gateway still has to subscribe to `roclink/out/<net>/<its address>/down`, offer each frame (sealing it with `roclink_sec_seal()`, bound as the record says) and publish `down_status`.
- **Two anti-replay windows.** Ingest and the router each keep their own, since each has its own MQTT session. They see the same frames and reach the same verdicts; if one process restarts with an older state file (restored from a backup, say), the two can disagree until the node's counter passes both.

## The hub as a security control

Each gateway refuses a frame counter it has already accepted, but only its own. A secured frame recorded near one gateway and replayed to another that never heard it would pass that gateway's check, so the hub keeps a window over every gateway ([PROTOCOL.md §10.5](../roclink-rfc/docs/PROTOCOL.md#105-frame-counters), `roclink_hub/replay.py`):

- per `(net, node, key_gen)`: the highest frame counter accepted, and a 64-bit map of the counters just below it, as IPsec does (RFC 4303);
- above the highest, or in the window and unseen: **new** (gateways can forward one node's frames out of order);
- in the window and seen: a **duplicate**, another gateway's copy, stored as a reception and never delivered again;
- below the window: a **replay**, dropped and counted as a `replay` rejection.

Ingest and the router each keep their window in their own state file (`ingest-replay.db`, `router-replay.db` in `state_dir`), written before a new frame goes any further, so a restart doesn't reopen it. Unsecured uplinks keep the time window of [PROTOCOL.md Annex B](../roclink-rfc/docs/PROTOCOL.md#annex-b-unsecured-operation).

## Downlink

A service asks for downlink on `roclink/svc/<service>/down`. It may send to any node and schema its `[[route]]`s match: the same rule as for what it receives.

```json
{"net": 66, "node": "0x0210", "schema": 10, "payload": "01", "ack": true, "ttl_s": 600, "ref": "open-1"}
```

`ref` is the service's own id, echoed back. `ack` (default true) asks the node to confirm. `ttl_s` (at most `[downlink] ttl_s`) is how long it may wait for its node.

The hub answers on `roclink/svc/<service>/down_status`:

```json
{"ref": "open-1", "id": "9f2c41d07a3b5e18", "status": "queued", "seq": 144}
{"ref": "open-1", "id": "9f2c41d07a3b5e18", "status": "acked"}
```

`status` is `queued`, then one of `acked` (the node confirmed it), `sent` (no ACK was asked for), `failed` (it ran out of attempts or time), or straight away `refused` with a `detail`.

What the hub does with it ([PROTOCOL.md §9.1](../roclink-rfc/docs/PROTOCOL.md#91-what-the-hub-must-do), `roclink_hub/downlink.py`):
- **Numbering:** one persisted downlink `seq` per node, starting at a random value. A re-offer keeps its `seq`, even through another gateway, so the node can't run a command twice.
- **Gateway:** the one that most recently heard the node. A node the hub hasn't heard from yet waits for its first uplink.
- **When:** a sleepy node's downlink is offered for its next listen window (`"send": "window"`), for up to `offer_window_s`. An always-listening node's is sent at once (`"send": "now"`), bound to the frame counter of the node's most recent uplink the hub has seen (`"binding"`).
- **Re-offers:** when a gateway reports `expired`, or doesn't answer by the offer's deadline, the hub offers it again through whichever gateway heard the node last, up to `max_attempts` and `ttl_s`.

The queue, the per-node counters and the last gateway and frame counter heard from each node live in `downlink.db`, this process's own file.

## Moving to Rust## Moving to Rust

The process boundaries map directly to tasks:

| Python | Rust |
|---|---|
| ingest and router processes + paho | two `rumqttc` tasks with their own sessions |
| `multiprocessing.Queue(maxsize)` | `tokio::sync::mpsc::channel(cap)`, the same backpressure |
| a writer process per database | a task (or thread) per database that owns its `rusqlite::Connection` |
| api processes | `axum`, one listener per database, with read-only connection pools |
| `record.Message` / `Uplink`, `config.Route` | `struct`s with `serde`, same fields |

The schema, the topic layout, the forward record format and the test vectors stay the same. Keep `tests/` as the conformance suite.
