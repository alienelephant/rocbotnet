# rfidprox ↔ backend: roclink messages and rfidauth

| | |
|---|---|
| Status | Draft |
| Version | 1 |

The proxy is a roclink node of device type `rfidprox`, always-listening, sending at level `enc`. Its messages to and from rfidauth use schemas 1–31; box data uses 32–255 (DESIGN.md §7.1). Integers are little-endian. Every payload starts with a version byte (1).

## 1. Proxy → rfidauth (uplink)

| Schema | Name | Payload after `ver` | Sent |
|---|---|---|---|
| 1 | `HELLO` | `fw_version` u16, `radio` u8 (1 LoRa, 2 Wi-Fi UDP, 3 ESP-NOW, 4 HaLow), `box_type` (length-prefixed, ≤ 16; empty if no box) | At boot and on reconnecting. `ACK_REQ`. |
| 2 | `AUTH_REQ` | `req` u8, `method` u8 (0 UID), `atqa` u16, `sak` u8, `uid_len` u8, `uid` (4, 7 or 10 bytes) | On a fob tap. `ACK_REQ`. |
| 3 | `RENEW` | `session` u16 | At two thirds of the lifetime. `ACK_REQ`. |
| 4 | `SESSION_END` | `session` u16, `reason` u8 (BOXLINK.md §4.2) | When the proxy or the box ends a session. `ACK_REQ`. |
| 5 | `STATUS` | `state` u8, `session` u16, `box` u8 (0 none, 1 present), `buffered` u8 | Every 5 minutes, and on a state change. |

The link heartbeat is roclink's own `PING` (schema 0), which the gateway ACKs and the hub uses for liveness (PROTOCOL.md §7.1).

## 2. rfidauth → proxy (downlink)

| Schema | Name | Payload after `ver` |
|---|---|---|
| 16 | `GRANT` | `req` u8, `result` u8 (§2.1), `session` u16, `lifetime_s` u16, `budget` u8 (messages per minute), `verdict` u8, `arg` u16, `label` (length-prefixed, ≤ 24) |
| 17 | `RENEWED` | `session` u16, `lifetime_s` u16 |
| 18 | `SESSION_CLOSE` | `session` u16, `reason` u8 |
| 19 | `CHALLENGE` | reserved for challenge-response cards (DESIGN.md §7.4) |

All are sent with `ACK_REQ`, at once (always-listening class).

### 2.1 GRANT results

| Result | Meaning | `verdict`, `arg` |
|---|---|---|
| 0 | granted | the app's verdict (e.g. scavhunt `go`, hints taken) |
| 1 | unknown fob | |
| 2 | fob disabled | |
| 3 | denied by the app | the app's verdict and argument (e.g. `locked`, needs box 0x0201) |
| 4 | busy: another session is open on this box | |
| 5 | backend unavailable (authenticator or app didn't answer) | |
| 6 | this proxy isn't assigned to a box | |

`label` is a short text for the proxy's display: the user's name and the app's label ("Alice · Red"), or why it was refused.

## 3. Box data (schemas 32–255)

| Bytes | Field |
|---|---|
| 0–1 | `session` u16 |
| 2 … | the box's payload, unchanged |

Both directions. The app drops uplink whose session isn't open for that node; the proxy drops downlink whose session isn't its current one.

## 4. Timing

| Setting | Default | Notes |
|---|---|---|
| `AUTH_REQ` timeout | 10 s | No `GRANT` by then: "try again", back to `idle` |
| Grant lifetime | 300 s | Renewed at 200 s |
| Heartbeat (`PING`) | 30 s while `active`, 120 s while `idle` | Three missed ACKs: link down |
| Grace | 60 s | DESIGN.md §6.3 |
| Retries | roclink's: same bytes, 3 times, 1.5 s apart over LoRa | |

These are first guesses (DESIGN.md §10, question 2).

## 5. rfidauth

### 5.1 Configuration

`boxes.toml`: which app owns each proxy.

```toml
[[box]]
node = "0x0201"   # the proxy's roclink address
app = "scavhunt"
box = "vault"     # the app's own id for it
```

`fobs.toml`: the stub authenticator's registry.

```toml
[[fob]]
uid = "04a1b2c3d4e5f6"
user = "alice"
name = "Alice"
enabled = true
```

### 5.2 The authenticator interface

```python
class Authenticator(Protocol):
    def authenticate(self, uid: bytes, card: CardInfo) -> Identity | Refusal: ...
    # Identity: user id, display name, claims (dict)
    # Refusal: unknown | disabled | unavailable
```

The stub (`StubAuthenticator`) reads `fobs.toml` and reloads it when the file changes. A real backend (HTTP, LDAP, a membership system) implements the same interface.

### 5.3 Handling `AUTH_REQ`

1. Look up the proxy in `boxes.toml`. Not there: result 6.
2. Another session is open on this node: result 4.
3. `authenticate(uid)`. Unknown or disabled: result 1 or 2. No answer within 3 s: result 5.
4. Send an authz `request` (`op: open`) to the app (DESIGN.md §7.3). No reply within 3 s: result 5. `deny`: result 3, with the app's verdict.
5. `go`: create the session (random 16-bit id, not 0, not in use on this node), store it, send `GRANT` result 0, and publish `session: open` to the app.

Every step is logged with a hash of the UID, never the UID itself.

### 5.4 Sessions

rfidauth stores each session (id, node, user, app, opened, expires, state) in SQLite. A session expires `lifetime_s` after its last grant or renewal. It's closed on `SESSION_END`, expiry, a new `HELLO` from the node, or a revocation by the app (`roclink/authz/<app>/revoke` with `{session, reason}`). Each close is published to the app as `session: closed`.
