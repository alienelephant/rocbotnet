# roclink gateway interface (draft)

**Status:** draft, and a separate RFC from the wire protocol. [PROTOCOL.md](../../roclink-rfc/docs/PROTOCOL.md) is normative for everything between nodes and gateways; this document drafts what happens next: how a gateway hands frames to the rest of the system, and what it accepts back. It's deliberately kept apart so the two can change at different speeds: the wire protocol is meant to stay stable, while this interface, and above all the hub behind it, are expected to change.

## 1. Scope and roles

```
 node ⇄ gateway ──MQTT──► broker ──► hub (any number of services)
 └─ PROTOCOL.md ─┘ └──────── this document ───────┘ └── open: not specified ──┘
```

- **The gateway** speaks the wire protocol to nodes. It opens and verifies frames, answers nodes, and publishes what it accepted and what it dropped.
- **The broker** is an MQTT server. Its ACL decides who may publish and read each topic.
- **The hub** is whatever consumes the gateways' messages. This document fixes only the messages and their data model. What a hub stores, how it routes to services, how it decodes payloads, alerts or exposes an API is left open, so hubs can differ and change. `hub/` in this repository is one demo implementation, not a definition.
- **What every hub must still do** is set by the wire protocol, because the protocol's security depends on it: number downlink per node, refuse replays across gateways, and keep control messages from applications. See [PROTOCOL.md §9.1](../../roclink-rfc/docs/PROTOCOL.md#91-what-the-hub-must-do).

**Conventions.** All messages are JSON objects. Ids may be numbers or `"0x…"` strings. Times are unix milliseconds on the sender's clock. Byte strings are hex. Every record carries a version `v` (1 by default); a receiver drops a version it doesn't know.

## 2. Topics

| Topic | Message |
|---|---|
| `roclink/in/<net>/<gateway>/up` | One accepted uplink: an uplink record (§3) or, from a bench bridge, a raw unsecured frame |
| `roclink/in/<net>/<gateway>/rejected` | One drop report (§4) |

- `<net>` is two hex digits and `<gateway>` four, in lowercase: `roclink/in/42/ff01/up`.
- Publish at QoS 1.
- Only gateways may publish under `roclink/in/`, enforced by the broker's ACL.
- Where uplinks go next (storage, services, a public feed, alerts) is the hub's business, and this document doesn't define it.
- Over a serial link, use the same JSON, one record per line.

All records are JSON objects:
- Ids may be numbers or `"0x…"` strings.
- Times are unix milliseconds on the gateway's clock.
- Byte strings are hex.

## 3. Uplink record

```json
{"v": 1, "gateway": "0xFF01", "time": 1791460800123,
 "net": 66, "src": "0x1234", "dst": 0, "seq": 7, "schema": 1, "flags": 3,
 "sec": "enc", "fcnt": 1, "key_gen": 0,
 "payload": "29091810cd8b",
 "radio": {"transport": "lora", "rssi": -87, "snr": 7.5}}
```

| Field | Required | Meaning |
|---|---|---|
| `v` | no (1) | Record version |
| `gateway` | no | The forwarding gateway. Defaults to the one in the topic. |
| `time` | no | When the gateway received the frame |
| `net`, `src`, `seq`, `schema` | yes | Envelope fields ([PROTOCOL.md §3](../../roclink-rfc/docs/PROTOCOL.md#3-frame-format)) |
| `dst`, `flags` | no (0) | Envelope fields |
| `sec` | no (`"none"`) | Security level: `none`, `auth` or `enc` ([PROTOCOL.md §10.1](../../roclink-rfc/docs/PROTOCOL.md#101-levels-and-policy)) |
| `fcnt`, `key_gen` | if secured | From the security header ([PROTOCOL.md §10.2](../../roclink-rfc/docs/PROTOCOL.md#102-secured-frame-layout)) |
| `session` | if in a session | The session's `join_fcnt` ([PROTOCOL.md §10.8](../../roclink-rfc/docs/PROTOCOL.md#108-sessions)). Session counters restart at 0, so replay checks across gateways are kept per session. |
| `payload` | yes | The application payload, decrypted |
| `radio` | no | `transport` (e.g. `lora`, `espnow`, `udp`), `rssi` (dBm), `snr` (dB) |

A gateway publishes a DATA frame only after accepting and verifying it ([PROTOCOL.md §9](../../roclink-rfc/docs/PROTOCOL.md#9-gateway-server-rules) rule 1, [§10.7](../../roclink-rfc/docs/PROTOCOL.md#107-receiving-a-frame)), and only once: not its retransmissions ([PROTOCOL.md §9](../../roclink-rfc/docs/PROTOCOL.md#9-gateway-server-rules) rule 2). A `PING` is published too, as a record on schema 0 with payload `01`; `JOIN` frames are not. Several gateways may publish the same uplink. The hub must remove duplicates and replays across gateways (PROTOCOL.md §9.1), for example with a per-node window over `(net, src, key_gen, session, fcnt)` ([PROTOCOL.md §10.5](../../roclink-rfc/docs/PROTOCOL.md#105-frame-counters)), or by `(net, src, seq)` within a time window for unsecured nodes ([PROTOCOL.md Annex B](../../roclink-rfc/docs/PROTOCOL.md#annex-b-unsecured-operation)).

## 4. Drop reports

Most attacks on a roclink network show up as frames a gateway drops:
- forged frames fail authentication;
- recorded frames come back as replays;
- a cloned node causes conflicts;
- a stolen device claims a revoked address;
- downgrade attempts fall below policy;
- floods of invented addresses fill peer tables.

A gateway reports each drop. What the hub does with the reports (counting, alerting) is up to the hub.

```json
{"v": 1, "kind": "rejected", "gateway": "0xFF01", "time": 1791460800123,
 "reason": "auth", "count": 1,
 "net": 66, "src": "0x1234", "dst": 0, "seq": 9, "schema": 32, "flags": 130,
 "fcnt": 65536, "key_gen": 0, "len": 35,
 "frame": "20824234120000092030000001004ef92ec936ea001cddda893489a6b0b20e9f70437e",
 "radio": {"transport": "lora", "rssi": -41, "snr": 9.5}}
```

The example is test vector S6 ([PROTOCOL.md §11](../../roclink-rfc/docs/PROTOCOL.md#11-test-vectors)) with its last tag byte changed, so the tag doesn't verify. Its header claims node `0x1234`.

| Field | Required | Meaning |
|---|---|---|
| `v` | no (1) | Record version |
| `kind` | yes | `"rejected"` |
| `gateway`, `time` | as in §3 | |
| `reason` | yes | Why the frame was dropped (table below). Lowercase letters and `_`. A receiver treats a reason it doesn't know as `warn`. |
| `count` | no (1) | More than 1 when the gateway has aggregated: this many frames with the same reason and `src` since its last report for them. The sample fields describe one of them. |
| `net`, `src`, `dst`, `seq`, `schema`, `flags`, `fcnt`, `key_gen` | as decoded | Whatever of the header decoded. Omitted if the frame didn't get that far. |
| `len` | no | Frame length in bytes |
| `frame` | no | The frame, or its first 64 bytes, as a sample |
| `radio` | no | As in §3 |

**Reasons.** Each reason corresponds to a library status code. "Report" says whether gateways must report it, should report it, or may only count it:

| `reason` | C status | Dropped because | Report |
|---|---|---|---|
| `malformed` | `ROCLINK_E_SHORT`, `_TOO_LONG`, `_VERSION`, `_TYPE`, `_MALFORMED` | It doesn't decode | should |
| `net` | `ROCLINK_E_NET` | Another network's frame | may count only |
| `addr` | `ROCLINK_E_ADDR` | Not addressed to this gateway, or `src` isn't a node | may count only |
| `relayed` | `ROCLINK_E_RELAYED` | Relayed frames are reserved in v1 | may count only |
| `unexpected` | `ROCLINK_E_UNEXPECTED` | Not expected now, e.g. an ACK with nothing pending | may count only |
| `policy` | `ROCLINK_E_POLICY` | Below the security policy: too low a level (e.g. unsecured from a node not allowed `none`) or too short a tag | should |
| `key` | `ROCLINK_E_KEY` | Unsupported suite or unknown key generation | should |
| `revoked` | `ROCLINK_E_REVOKED` | It claims a revoked node | **must** |
| `not_enrolled` | `ROCLINK_E_NOT_ENROLLED` | Unknown node, and admission isn't open | should |
| `epoch` | `ROCLINK_E_EPOCH` | Its epoch is beyond the ratchet limits ([PROTOCOL.md §10.4](../../roclink-rfc/docs/PROTOCOL.md#104-keys)) | should |
| `replay` | `ROCLINK_E_REPLAY` | `fcnt` below the last accepted ([PROTOCOL.md §10.5](../../roclink-rfc/docs/PROTOCOL.md#105-frame-counters)) | should |
| `auth` | `ROCLINK_E_AUTH` | The tag didn't verify | should |
| `conflict` | `ROCLINK_E_CONFLICT` | An authentic frame reused a counter with different content ([PROTOCOL.md §10.5](../../roclink-rfc/docs/PROTOCOL.md#105-frame-counters)) | **must**, at once |
| `no_session` | `ROCLINK_E_NO_SESSION` | A session-mode node's frame, and this gateway has no session with it: it restarted, or the node joined another gateway ([PROTOCOL.md §10.8](../../roclink-rfc/docs/PROTOCOL.md#108-sessions)) | may count only |
| `peers_full` | `ROCLINK_E_PEERS_FULL` | The gateway's secured peer store is full and has no entry for this node (PROTOCOL.md §9 rule 2). The unsecured duplicate table evicts instead and never causes this. | should |

A retransmission (`ROCLINK_RX_DUPLICATE`) and schema-0 control frames are not drops and aren't reported.

**Rules:**
1. **Nothing in a report is verified,** except the reason.
   - Every field comes from a frame the gateway refused, so whoever sent it chose it: `src` is the address the frame *claims*.
   - The exception is `conflict`, which is decided only after the tag verifies ([PROTOCOL.md §10.7](../../roclink-rfc/docs/PROTOCOL.md#107-receiving-a-frame) step 7). So the header of a `conflict` frame is authentic.
   - `revoked` is decided before any cryptography (step 2). Anyone can cause one by claiming a revoked address, so it's a lead to follow up, not proof that the stolen device is nearby.
   - Receivers must treat reports as attacker-controlled data, for example when showing them on a dashboard.
2. **A gateway must bound its report rate.** Each forged frame costs the attacker one transmission. Without a bound, a flood of them becomes a flood of broker messages and fills the gateway's onward link.
   - Beyond a configured rate, a gateway aggregates: one report per `(reason, src)` with `count`, carrying one sample.
   - Reasons that may only be counted are counted by reason alone, without `src`: on a shared band, other networks' addresses are many and of no interest one by one.
   - It reports `conflict` at once, since only a key holder can cause one.
   - `roclink-gw` in the roclink-gateway repository does all of this (its `dropped()` and `flush_reports()`). Each interval (10 s by default), it reports the first drops (10 by default) one by one, then counts the rest and reports the counts when the interval ends.
3. **Reports are private.** A frame dropped for `policy` may carry plaintext from a misconfigured device. A gateway publishes reports only on its `rejected` topic, which needs the same protection as its `up` topic.
4. **Reporting changes nothing.** A dropped frame changes no protocol state ([PROTOCOL.md §10.7](../../roclink-rfc/docs/PROTOCOL.md#107-receiving-a-frame)), and neither does its report. The report is sent after the drop decision, and never delays a reply to a node.
5. **Include radio metadata where the carrier has it.** Signal strength is often the best evidence. A node that is usually received at −95 dBm whose frames suddenly fail authentication at −40 dBm points to a transmitter nearby.

## 5. Downlink

Whatever originates downlink (the hub) numbers it per node and hands each frame to one gateway (PROTOCOL.md §9.1). The gateway offers it to the node and reports what happened.

| Topic | Message |
|---|---|
| `roclink/out/<net>/<gateway>/down` | One downlink request, hub to gateway |
| `roclink/in/<net>/<gateway>/down_status` | What became of it, gateway to hub |

```json
{"v": 1, "id": "7f3a", "node": "0x1234", "schema": 10, "seq": 144, "ack": true,
 "payload": "3c00", "send": "window", "binding": null, "expires": 1791460860000}
```

| Field | Meaning |
|---|---|
| `id` | The hub's id for this request, echoed in the status |
| `node`, `schema`, `payload` | As in §3. Services use schemas 1–255; schema 0 is the hub's own, for a `PING` (payload `01`) that checks an always-listening node is reachable (PROTOCOL.md §7.1). |
| `seq` | The downlink `seq` the hub assigned (PROTOCOL.md §9.1). A re-offer keeps it. |
| `ack` | Set `ACK_REQ` |
| `send` | `"window"`: offer it in the node's next reply window (sleepy node). `"now"`: send it at once (always-listening node). |
| `binding` | For `"now"` on a secured node: the `fcnt` of the node's most recent uplink the hub has seen ([PROTOCOL.md §10.6](../../roclink-rfc/docs/PROTOCOL.md#106-downlink-freshness)). Otherwise `null`. |
| `session` | For a session-mode node: the session the binding belongs to (its `join_fcnt`, as in §3). A gateway that no longer has that session reports `expired`. |
| `expires` | Unix ms after which the gateway gives up and reports `expired` |

The status is `{"v": 1, "id": "7f3a", "status": "acked"}`, with `status` one of `acked`, `sent` (no ACK was requested) or `expired`. Only the hub may publish under `roclink/out/`, enforced by the broker's ACL.

## 6. Open questions

1. **Normative or not.** Should this interface become normative, so gateways and hubs by different authors are guaranteed to work together, or stay a recommended format? A gateway may forward in any format today (PROTOCOL.md §9 rule 7).
2. **Serial form.** A gateway with no network (an ESP32 on a Raspberry Pi's USB port, say) needs a byte-stream form. JSON lines, one record per line, are the obvious candidate; PROTOCOL.md §14 item 2 asks the related question of framing roclink frames themselves over a stream.
3. **Transports other than MQTT.** Whether to define the same records over HTTP or a WebSocket, for hubs that don't run a broker.
4. **Liveness and status of gateways.** Whether gateways publish their own heartbeat and status (uptime, radio settings, queue depth), which a hub needs to choose between gateways for downlink.
