# roclink wire protocol, v1

This document is normative. The C library in `src/` implements it, but you don't need the library to take part. A client or server written in any language interoperates if it follows this document and reproduces the [test vectors](#11-test-vectors) byte for byte.

**Status:** draft for review (roclink 0.3), wire format v1.

**Scope:** nodes and gateways: the frames between them, and the rules each follows. What a gateway does with frames afterwards, and the system behind it (*the hub*), are out of scope: GATEWAY_INTERFACE.md drafts how gateways talk to a hub, as a separate RFC, and the hub's design is left open. The wire protocol needs only a few things from the hub, all listed in [§9.1](#91-what-the-hub-must-do).

The wire format is meant to stay stable. New features use the code points it reserves for them: frame types, schema-0 control operations ([§7.1](#71-control-messages)) and security suites ([§12](#12-reserved-space-and-versioning)).

The library in `src/` implements it, including the security extension ([§10](#10-security)) and the 0.3 changes listed in [§13.1](#131-changes-in-03), including the node's `PING` ([§7.1](#71-control-messages)), except session mode ([§10.8](#108-sessions)), which is specified but not built yet. Open questions are collected in [§14](#14-open-questions), and work left for later in [§15](#15-future-work). The threat model is in [SECURITY.md](SECURITY.md).

## 1. Model

- **One envelope for every carrier.** The same frame bytes travel over LoRa, ESP-NOW, nRF24, Wi-Fi/UDP or a serial line. A gateway reads only the envelope and forwards the payload without interpreting it.
- **Nodes and gateways.** *Nodes* (clients) are devices around a building that send their own data. *Gateways* (servers) receive it and forward it to *the hub*, whatever system sits behind them; its design is out of scope ([§9.1](#91-what-the-hub-must-do)). A deployment can have several gateways, each on whatever radio suits its area.
  ```
  node ⇄ gateway ──► hub
  └─ this spec ─┘   └ GATEWAY_INTERFACE.md (draft), hub design open ┘
  ```
- **Two node classes.** A node transmits whenever it likes. A gateway may transmit to it only while it is listening:
  - **Sleepy nodes** (the default, for batteries) listen only right after a node frame that says so (`LISTEN` or `ACK_REQ`). Between transmissions they can turn the radio off.
  - **Always-listening nodes** (mains-powered, typically actuators) keep their receiver on, so the gateway may send them downlink at any time ([§7](#7-exchanges)).

  A node's class is configuration shared by the node and the hub. It isn't sent on the wire.
- **Two ways to key a node.** By default every secured frame uses the node's long-term chain key and a frame counter the node persists. A node can instead run in *session mode*: at each boot it joins a gateway and both derive a fresh session key, so its frame counters restart at 0 and nothing persists per frame ([§10.8](#108-sessions)). Session mode suits mains-powered nodes; sleepy battery nodes keep the default.
- **Opaque payload.** Each DATA frame carries one application payload, tagged with an 8-bit **schema** that tells the receiver what format it is in. roclink never looks inside a payload, and nothing in the protocol knows about sensors or hardware.
- **Reliability per message.** Fire-and-forget by default. Any frame can request an ACK, and the sender then retransmits until it is acknowledged or it gives up.
- **Secure by default, insecure by explicit choice.** A frame is authenticated, or authenticated and encrypted, with AES-128-CCM under a per-node key that ratchets forward each epoch, so a stolen device reveals no earlier traffic ([§10](#10-security)). A node can be configured without security, like HTTPS with certificate checks switched off. That is always an explicit choice made for *that node*, or for an open test net that has no secured nodes, and the *receiver's* policy decides what it accepts, so an attacker can't downgrade a link ([Annex B](#annex-b-unsecured-operation)).

## 2. Transport requirements

A transport moves whole frames. To carry roclink it must:

1. **Deliver a frame intact or not at all, and never truncate it.** roclink's authentication tag detects tampering but isn't an error-detecting code, and an unsecured frame has no check at all. Because there is no length field, a carrier that silently shortens a frame (for example an ESP-NOW v1 receiver given a frame over 250 B) produces an unsecured frame that looks valid; a secured one fails its tag check. Packet radios already check integrity (LoRa payload CRC, the 802.11 FCS under ESP-NOW, the nRF24 CRC). A byte-stream transport such as a UART or TCP must add its own framing and integrity check.
2. **Preserve frame boundaries and length.** There is no length field. The payload runs to the end of the frame.
3. **Carry at least 9 bytes**, or 22 for a secured frame with an 8-byte tag. A transport's maximum frame size limits the payload a node can send over it ([§3](#3-frame-format)). For example, an nRF24 (32 bytes) leaves 10 bytes of secured payload.

Timing is also the transport's business. That covers how long a node keeps listening, how soon a gateway must reply, and airtime or duty-cycle rules. The protocol only says *when a reply is permitted*.

### 2.1 Sharing a radio with other protocols

roclink has no protocol identifier in its header, by design. Whether a frame is roclink is settled by the carrier it arrives on, then by `netId`, the address checks and, for secured frames, the tag. So:

- **Give roclink its own carrier settings:** its own LoRa frequency or sync word, ESP-NOW Wi-Fi channel, UDP port, or nRF24 channel and address. A radio only hears its own modulation and settings, so this keeps other protocols' packets away from roclink entirely.
- **If roclink does share a channel** with another protocol (MySensors on the same LoRa settings, other ESP-NOW applications on the same Wi-Fi channel), each side receives the other's packets. They don't get mistaken for data: a foreign frame passes roclink's header checks very rarely (version, type, `netId` and a destination of this gateway or `0x0000` together match about 1 in 300 million random frames), and a secured frame must also pass its tag. They are dropped and counted as `malformed`, `net` or `addr`. What they do cost is airtime: two protocols on one channel corrupt each other's packets when they overlap, whatever their headers say.
- **On 2.4 GHz, plan channels** so Wi-Fi and ESP-NOW, 802.15.4 (Thread, Zigbee), BLE and nRF24 don't overlap: for example Wi-Fi on 1, 6 or 11, and 802.15.4 on 15, 20, 25 or 26.
- **One gateway host can serve several protocols** with one radio and one gateway stack per protocol, each forwarding upstream separately. Matter is the exception: Matter devices are IP hosts with end-to-end encryption, so a host serving them runs a Thread border router, which routes their packets without reading them.

Transport security (WPA2, ESP-NOW encryption, TLS) may be used as well, but roclink doesn't rely on it. LoRa and nRF24 have none. A gateway's onward link (MQTT, a host connection) should always use TLS or equivalent.

## 3. Frame format

All multi-byte fields are little-endian.

```
 byte   0         1       2       3   4    5   6    7      8        [9  10]
      +---------+-------+-------+--------+--------+------+--------+-----------+------------------+---------+------+
      | ver|type| flags | netId |  src   |  dst   | seq  | schema | relay id  | security header  | payload | tag  |
      +---------+-------+-------+--------+--------+------+--------+-----------+------------------+---------+------+
                                                                   if RELAYED   if SECURED (5 B)              if SECURED
```

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `ver` (bits 7..5), `type` (bits 4..2), reserved (bits 1..0) | `ver` = 1. Reserved bits are sent as 0 and ignored on receive. |
| 1 | 1 | `flags` | See [§6](#6-frames-and-flags). |
| 2 | 1 | `netId` | Network id. Frames from other networks are dropped. |
| 3 | 2 | `src` | Sender address ([§4](#4-addresses)). |
| 5 | 2 | `dst` | Destination address. |
| 7 | 1 | `seq` | Sender's sequence number. An ACK echoes the acknowledged frame's `seq`. |
| 8 | 1 | `schema` | Payload format, chosen by the application ([§5](#5-message-schemas)). An ACK uses 0. |
| 9 | 2 | `relay` | Present only when `RELAYED` is set (reserved). |
| next | 5 | security header | Present only when `SECURED` is set ([§10.2](#102-secured-frame-layout)). |
| next | 0..246 (jumbo: more) | payload | Encrypted when the security level is `enc`. |
| last | 4 or 8 | tag | Present only when `SECURED` is set. |

The base header is **9 bytes** (11 when relayed).

**Why `seq` is 8 bits.** Secured frames detect duplicates by `fcnt`, so `seq` only matches ACKs to frames and removes duplicates for unsecured nodes, where an unpersisted reboot has a 1-in-256 chance of its first frame being taken as a duplicate ([Annex B](#annex-b-unsecured-operation)). A wider `seq` would cost a header byte, which matters at LoRa SF10 in US915 (15 B of payload) and on nRF24.

**Frame size.** Every implementation must accept frames of up to **255 bytes**, the *standard* size, which is also the LoRa limit. An implementation may also support **jumbo frames** up to **65,535 bytes**, the most AES-CCM can protect with a 13-byte nonce, for carriers that can carry them (ESP-NOW v2: 1470 B; UDP: 1472 B without IP fragmentation). The frame format is the same; only the length differs, and the carrier reports it.

| | Standard (255 B) | Jumbo, e.g. 1470 B |
|---|---|---|
| Unsecured | 246 B | 1461 B |
| Secured, 8-byte tag | 233 B | 1448 B |
| Secured, 4-byte tag | 237 B | 1452 B |

Rules for jumbo frames:
- **The receiver decides.** A receiver drops any frame longer than the size it supports. A standard receiver drops every jumbo frame, rather than handing an unexpectedly large payload to a small device.
- **Only where every receiver supports them.** Which carriers and devices may use jumbo frames is a deployment decision, not something the protocol negotiates.
- **Level `auth` is limited to the standard 255 B frame** ([§10.3](#103-cryptographic-operation)). Jumbo frames are `none` or `enc`.
- **A gateway forwarding between carriers** must not send a frame larger than the next carrier accepts.

A transport may allow less than the standard size (nRF24: 32 B).

## 4. Addresses

| Value | Meaning |
|---|---|
| `0x0000` | **Any gateway.** Used only as `dst` in a node's uplink: whichever gateway hears it may take it. |
| `0x0001`–`0xFEFF` | **Nodes.** |
| `0xFF00`–`0xFFFE` | **Gateways.** Each gateway has its own address in this range. |
| `0xFFFF` | Broadcast. Reserved in v1, so receivers drop frames sent to it. |

Because the ranges are separate, any receiver can tell from `src` whether a frame came from a node or a gateway, by its high byte alone. That's 255 gateways per network, and the `netId` separates sites, so a larger range would only take addresses from nodes, which have 65,279.

Every node and gateway address on a secured network **must be assigned and unique**. A node's key is derived from its address ([§10.4](#104-keys)), so two devices sharing an address would share a key and could reuse a nonce, which breaks CCM. Unsecured nodes may derive their address from hardware instead ([Annex B](#annex-b-unsecured-operation)).

## 5. Message schemas

The *message schema* byte (`schema`) tells the receiver what the payload means: which layout it has and what its fields are. It's the same idea as LoRaWAN's FPort, renamed because it names a message format, not an endpoint. The application owns the schema space:

| Schema | Use |
|---|---|
| 0 | roclink control messages, such as `PING` ([§7.1](#71-control-messages)). Receivers never deliver schema-0 DATA to the application. They still ACK it if asked. |
| 1–255 | Application-defined, **per device type**. |

**Schemas are scoped per device type and per direction.** Every node belongs to a *device type*, the firmware and data model one maker wrote. Whoever writes a device type owns schemas 1–255 for it, so makers don't have to coordinate schema numbers with each other. Whatever reads payloads behind the gateways therefore needs to know each node's device type, and interprets a payload by `(device type, schema)`. Schema 1 can be an environment reading on one maker's board and a door event on another's, and schema 1 on downlink can mean something else again.

Eight bits are enough because of that scoping: each device type has its own 255 formats, and no deployment-wide registry is needed.

A device type typically assigns one schema per payload format, for example environment, event, power and diagnostics, and puts a version byte at the start of its own payloads if it expects them to change. See USE_CASES.md for examples.

The schema is part of the readable envelope, so it reveals the *kind* of data even when the payload is encrypted ([SECURITY.md](SECURITY.md)).

## 6. Frames and flags

| `type` | Name | Payload |
|---|---|---|
| 0 | DATA | Application bytes (may be empty). |
| 1 | ACK | None. `seq` is the acknowledged frame's `seq`, `schema` is 0. Receivers drop an ACK that carries a payload or a non-zero `schema`. |
| 2 | JOIN | Starts a session ([§10.8](#108-sessions)): a 1-byte operation, then its body. Always secured, at level `enc`. |
| 3–7 | Reserved | Receivers drop these. |

| Bit | Mask | Name | Set by | Meaning |
|---|---|---|---|---|
| 0 | `0x01` | `ACK_REQ` | node, gateway | The receiver must ACK this frame. On a node's frame it also means `LISTEN`. |
| 1 | `0x02` | `LISTEN` | node | The node listens for replies right after this frame. |
| 2 | `0x04` | `MORE` | gateway | Another gateway frame follows straight after this one, so keep listening. |
| 3 | `0x08` | `DL_QUEUED` | gateway | More downlink is waiting. Transmit with `LISTEN` again soon to fetch it. |
| 4 | `0x10` | `RELAYED` | (reserved) | A 2-byte relay id follows the header. |
| 6..5 | `0x60` | `HOPS` | (reserved) | Relay hop count. |
| 7 | `0x80` | `SECURED` | node, gateway | A security header follows, and a tag ends the frame ([§10](#10-security)). |

Senders must set reserved bits to 0. Receivers must ignore reserved bits they don't understand, except `RELAYED` and `SECURED`, which change the frame layout.

## 7. Exchanges

**Fire-and-forget uplink.** The node sends DATA with no flags. The gateway delivers it and sends nothing.

**Uplink with ACK.**
```
node     |== DATA  ACK_REQ ==|   (listening)  [========]
gateway                      |..... reply .....|== ACK ==|
```
If no ACK arrives, the node sends the *same bytes* again. The gateway acknowledges every copy and delivers only the first ([§9](#9-gateway-server-rules)).

**Downlink in a listen window.** The node sends DATA with `LISTEN`. If the gateway holds downlink for that node, it replies with one DATA frame. If that frame has `ACK_REQ`, the node answers with an ACK.

**ACK followed by downlink.**
```
node     |== DATA ACK_REQ ==|       [====== listening ======]       |== ACK ==|
gateway                     |== ACK  MORE ==|== DATA ACK_REQ ==|
```
`MORE` on the ACK tells the node to keep listening for the DATA frame.

**Unsolicited downlink (always-listening nodes only).**
```
node     ......... receiver always on ...............................  |== ACK ==|
gateway                  |== DATA ACK_REQ ==|
```
The gateway sends DATA without waiting for an uplink. If it has `ACK_REQ`, the node answers with an ACK. Secured, the frame is bound to the node's most recent uplink ([§10.6](#106-downlink-freshness)).

**Rules:**
- A gateway transmits to a **sleepy** node only in reply to that node's `LISTEN` or `ACK_REQ` frame. That node frame *opens the reply*, and secured gateway frames are bound to it ([§10.6](#106-downlink-freshness)).
- To an **always-listening** node, a gateway may also send downlink at any time, when the hub tells it to ([§9](#9-gateway-server-rules) rule 9).
- A gateway sends at most one ACK and one DATA frame per reply. If more downlink remains, it sets `DL_QUEUED`.
- A node transmits only its own uplink and the ACKs of downlink it received.
- How long the node listens, and how quickly the gateway must answer, is set by the transport ([§2](#2-transport-requirements)).

### 7.1 Control messages

A control message is a DATA frame with schema 0. Its payload is a 1-byte operation, then that operation's body. It's secured like any other frame, and the receiver's policy applies to it in the same way. Receivers ignore body bytes they don't understand, so later versions can add fields.

| Operation | Name | Body | Direction |
|---|---|---|---|
| `0x00` | Reserved | | |
| `0x01` | `PING` | none in v1 (later versions may add status fields) | node → gateway, or gateway → node |
| `0x02`–`0xFF` | Reserved | | receivers drop the frame (`malformed`) and don't ACK it |

**`PING`** is the keepalive. It carries no data; what it does depends on its flags:

| Sent by | Flags | Means |
|---|---|---|
| node | none | "I'm alive." Refreshes the hub's view of the node and, for an always-listening node, its downlink binding (§10.6). |
| node | `ACK_REQ` | A link check. The gateway's ACK proves the node can still reach a gateway and, in session mode, that its session is still there. |
| node | `LISTEN` | A poll: "send me anything queued." Cheaper than a DATA frame when the node has nothing to report. |
| gateway | `ACK_REQ` | "Are you there?" Only to an always-listening node, or in a node's reply window. The node's ACK proves it's reachable. |

Rules:
- **Heartbeat:** an always-listening node sends a `PING` (or any DATA frame) at least once every heartbeat interval: 300 s by default, or the interval its session sets (§10.8). The hub may treat a node that misses three heartbeats as offline.
- **Gateways** ACK a `PING` when asked, and pass it on to the hub like any uplink, with schema 0. The hub may use it for liveness and for choosing the gateway for downlink, and never hands schema-0 messages to applications (§9.1).
- **Cost:** a `PING` is 10 bytes unsecured, 23 secured with an 8-byte tag.

## 8. Node (client) rules

1. **Sequence.** Use the next `seq` (mod 256) for each new DATA frame. A retransmission is the identical frame with the same `seq` (and, when secured, the same frame counter). Start from a random `seq` at cold boot. Across deep sleep, persist the next `seq`, so a gateway doesn't take the first frame after waking for a retransmission.
2. **Accept** a frame only if all of these hold:
   - it decodes ([§3](#3-frame-format));
   - `netId` matches;
   - `src` is a gateway address (and the node's configured gateway, if it has one);
   - `dst` is the node's own address;
   - `RELAYED` is clear;
   - it satisfies the node's security policy and verifies ([§10.7](#107-receiving-a-frame)).
3. **ACK:** it completes the pending send if its `seq` equals the `seq` of the frame awaiting an ACK. Otherwise it is stale and is ignored.
4. **DATA:** it is a duplicate if its `seq` equals the `seq` of the last DATA accepted, from any gateway. The hub numbers each node's downlink, so a downlink re-offered through a different gateway keeps its `seq` (gateway rule 3). Deliver it to the application unless it is a duplicate or on schema 0. If it has `ACK_REQ`, ACK it *even if it is a duplicate*, because a repeat means the previous ACK was lost.
5. Remember `DL_QUEUED` from the last *accepted* frame, and keep listening while `MORE` is set. A frame that is dropped, such as a stale ACK, changes neither.
6. **Addressing:** a node addresses `0x0000` (any gateway) by default. Where coverage overlaps, it can address one gateway directly (gateway rule 5).
7. **Always-listening nodes** accept gateway frames at any time, under the same checks. Secured, they verify each one against their most recent DATA frame ([§10.6](#106-downlink-freshness)). After a reboot, they accept nothing until they have sent one DATA frame. They send a heartbeat (§7.1), which also bounds how late a jammed downlink can arrive.
8. **Session-mode nodes** join before sending anything else, and re-join when the session ends: at its lifetime, after a reboot, or after three `ACK_REQ` frames in a row (DATA or `PING`) go unacknowledged, which means the gateway has lost the session ([§10.8](#108-sessions)).

## 9. Gateway (server) rules

1. **Accept** a frame only if all of these hold:
   - it decodes;
   - `netId` matches;
   - `src` is a node address;
   - `dst` is this gateway's address or `0x0000`;
   - `RELAYED` is clear;
   - it satisfies the gateway's security policy for that node and verifies ([§10.7](#107-receiving-a-frame)).
2. **Duplicates:**
   - **Secured frames:** a DATA frame whose frame counter equals the last one accepted from that node is a retransmission. A lower counter is a replay and is dropped ([§10.5](#105-frame-counters)).
   - **Unsecured frames** (opted-in nodes only): by `seq` within a time window ([Annex B](#annex-b-unsecured-operation)).
   - ACK a retransmission again if asked, but don't deliver it again.
   - A gateway's duplicate table may be smaller than the number of nodes. When it is full, a frame from an untracked node makes the gateway forget the least recently seen node. The secured peer store is different: it must hold every node the gateway accepts, persisted ([§10.5](#105-frame-counters)), and a node it has no room for is refused and reported as `peers_full`.
3. **Downlink sequence:** whatever originates downlink, normally the hub (§9.1), keeps one counter **per node**, persisted and starting at a random value, and gives each downlink its `seq`. Consecutive downlinks to a node then always differ, even through different gateways, so the node's duplicate check (node rule 4) can't swallow a new command, and a re-offer through another gateway can't be executed twice.
   - **To re-offer** an unacknowledged downlink, keep its `seq` and payload.
   - **Unsecured:** resend the identical frame.
   - **Secured:** seal it again with a new frame counter, bound to the node frame that opened the new reply, or for an always-listening node to its most recent uplink ([§10.6](#106-downlink-freshness)).
4. **Replies:** reply only when the node frame had `LISTEN` or `ACK_REQ`, and only as allowed by the next rule. Unsolicited downlink to always-listening nodes is the exception (rule 9).
5. **Multiple gateways.** A frame addressed to a specific gateway is answered only by that gateway. A frame addressed to `0x0000` may be heard by several:
   - every gateway that hears it passes it on, and the hub removes duplicates and replays across gateways (§9.1);
   - only gateways configured to *answer any* reply to it. Two gateways answering the same frame would collide on the air, so in areas of overlapping coverage, configure one gateway to answer, or have nodes address a gateway directly.
6. **ACK from a node:** `seq` is the acknowledged downlink's `seq`.
7. **Forwarding:** a gateway passes each accepted DATA frame on to the hub once (not its retransmissions), with the envelope fields, the decrypted payload and its security level. How is out of scope; GATEWAY_INTERFACE.md drafts the format roclink's own tools use.
8. **Reporting drops:** a gateway should report the frames it drops, with the reason (GATEWAY_INTERFACE.md §4). Dropped frames are where forgery, replay, cloned nodes and stolen devices show up, so the system behind the gateways needs to see them.
9. **Offering downlink:** downlink comes from the hub, already numbered (rule 3, §9.1). For a sleepy node, the gateway offers it in the node's next reply window; for an always-listening node it sends it at once, bound to the node uplink the hub names ([§10.6](#106-downlink-freshness)). A gateway holds downlink only while offering it, and tells the hub the outcome: ACKed, sent without an ACK request, or expired.
10. **Control messages:** a `PING` is acknowledged when asked and passed on to the hub (§7.1). A gateway that keeps sessions answers `JOIN` requests as it answers other frames (rule 5), and keeps each session in RAM only ([§10.8](#108-sessions)).

### 9.1 What the hub must do

The hub's design is open, but the wire protocol's guarantees rely on four things that only something behind every gateway can do. Any hub must:

1. **Give each node's downlinks their own sequence numbers.** Whatever originates downlink keeps a `seq` counter for every node, saves it across restarts, starts it at a random value, and gives each downlink the next `seq` (rule 3). A downlink that's retried, even through a different gateway, keeps its original `seq`, so the node recognises the repeat and doesn't act on it twice (node rule 4).
2. **Send each downlink frame through one gateway**, and for an always-listening node supply the binding: the `fcnt` of the node's most recent uplink it has seen, and in session mode the session it belongs to (§10.6, §10.8). A session-mode node's downlink goes through the gateway it joined.
3. **Refuse replays across gateways** when a network has more than one gateway. Each gateway refuses frame counters it has accepted, but only its own: a frame recorded near one gateway and replayed to another that never heard it passes that gateway's check. The hub must refuse a counter that any gateway has already passed on from that node, per `(net, node, key_gen, session)`, and must keep that state across restarts. Several gateways may pass on the same frame and in any order, so it must still accept a counter it hasn't seen yet. A sliding window, as IPsec uses (RFC 4303 §3.4.3), is one way to do this; the demo hub uses 64 entries.
4. **Keep control messages from applications.** Schema-0 messages (§7.1) are roclink's own: never deliver them to an application.

A deployment with one gateway and no hub, such as a bench setup, does these in the gateway.

## 10. Security

The threat model, assumptions and limits are in [SECURITY.md](SECURITY.md). This section is the wire format and the rules.

### 10.1 Levels and policy

| Level | `SECURED` | Payload | Tag covers |
|---|---|---|---|
| `none` | 0 | Plain | Nothing; no tag |
| `auth` | 1 | Plain | Envelope + payload |
| `enc` | 1 | AES-128-CCM ciphertext | Envelope + payload |

- **Sender:** a link's level is configured per network or per node. `enc` is the default. `none` must be chosen explicitly and should be logged as insecure.
- **`auth`:** for situations where payloads must stay readable, such as sniffing during development, or amateur-radio bands where encryption is prohibited.
- **Receiver policy** is configured per network, and can be overridden per node:

| Policy | Accepts | Set | Use |
|---|---|---|---|
| `require-enc` | `enc` only | network or node | **Default** |
| `require-auth` | `auth` or `enc` | network or node | |
| `allow-none` | Anything | **node**, or an **open net** | Explicit opt-out for one named node, e.g. on the bench, or a node built without crypto; or for a whole net that has no secured nodes ([Annex B](#annex-b-unsecured-operation)) |
| `deny` | Nothing | **node only** | Revokes a node. Its frames are refused at every level, and nothing is sent to it. |

- **Minimum tag length** is part of the policy: **8 bytes by default**, and it can be lowered to 4 for the network or for one node (§10.2). A frame with a shorter tag is dropped like a frame below the policy.
- A frame below the receiver's policy is dropped. This includes ACKs: an unsecured ACK for a secured frame is ignored. Because the receiver decides, an attacker can't downgrade a link by sending unsecured frames.
- **Network-wide `allow-none` only on an open net.** A gateway answers an unsecured frame unsecured. If a net that has secured nodes allowed `none` for everyone, anyone could send an unsecured `LISTEN` frame claiming a secured node's address and receive that node's queued downlink in plaintext, then forge an unsecured ACK so the hub believes it was delivered. So:
  - on a net with any secured node, `allow-none` is set per node, which limits this to nodes that have nothing to protect;
  - an **open net**, one with no secured nodes at all, such as the hub's public net for testing devices, may use `allow-none` as its network policy. A gateway holds no keys and no secured peers for an open net, and the hub never routes its traffic to anything that trusts it ([Annex B](#annex-b-unsecured-operation)).
- **Builds without crypto:** a node may be built without the security module. It can then only send `none`, and receivers must explicitly allow that node.

### 10.2 Secured frame layout

```
| base header (9 B, SECURED set) | [relay id] | sec_ctrl (1 B) | fcnt (4 B LE) | payload | tag (4 or 8 B) |
```

| Field | Bits | Meaning |
|---|---|---|
| `sec_ctrl` | 7..6 `suite` | 0 = AES-128-CCM. 1–3 are reserved; receivers drop frames with a suite they don't support. |
| | 5 `ENC` | 1 = level `enc`, 0 = level `auth`. |
| | 4 `TAG8` | 1 = 8-byte tag, 0 = 4-byte tag. |
| | 3..0 `key_gen` | Master key generation, for rotating the master key ([§10.4](#104-keys)). The ratchet epoch isn't sent: it comes from `fcnt`. |
| `fcnt` | 32 | The sender's frame counter ([§10.5](#105-frame-counters)). |

**Tag length:**
- **8 bytes** is the default, for senders and as the receiver's minimum (§10.1).
- **4 bytes** is allowed per link where frame size or airtime is tight, such as nRF24 or high-SF LoRa. Both ends must opt in: the sender is configured for 4, and the receiver's policy lowers its minimum to 4 for that node or network.
- An 8-byte tag gives an attacker a 1-in-2⁶⁴ chance per forgery attempt. A 4-byte tag gives 1-in-2³², the same as LoRaWAN's MIC. That's acceptable because each attempt costs airtime and is rate-limited by the radio.

### 10.3 Cryptographic operation

**Suite 0 is AES-128-CCM** (NIST SP 800-38C, RFC 3610), with:
- a 13-byte nonce, giving a 2-byte length field;
- a tag (CCM's "M") of 4 or 8 bytes, from `TAG8`.

**Nonce (13 bytes):**

| Bytes | 0–1 | 2–3 | 4 | 5–8 | 9–12 |
|---|---|---|---|---|---|
| Field | `src` (LE) | `dst` (LE) | `netId` | `fcnt` (LE) | `00 00 00 00` |

**Associated data (AAD), in this order:**
1. The base header (bytes 0–8), with `RELAYED` and `HOPS` cleared in `flags` (`flags & 0x8F`). The relay id is not included. These fields are the ones a future relay will change, so leaving them out keeps relaying compatible.
2. `sec_ctrl` and `fcnt` (5 bytes).
3. **Gateway frames only:** the 4-byte `fcnt` (LE) of the node frame that opened the reply ([§10.6](#106-downlink-freshness)). This value isn't transmitted; both sides know it.

**Sealing:**
- **`enc`:** CCM-encrypt the payload with the AAD above. The frame carries the ciphertext, then the tag.
- **`auth`:** run CCM with an *empty* plaintext and the payload appended to the AAD. The frame carries the plain payload, then the tag.

**Frame size:** an `auth` frame is at most 255 B in every implementation. Its whole payload is part of the AAD, and capping it keeps that bounded on small devices. An `auth` frame longer than 255 B is dropped. `enc` frames may be as long as the implementation's frame limit (§3), up to 65,535 B.

**Uniqueness rule:** a nonce must never repeat under a key. The nonce contains `src` and `fcnt`, and each sender's `fcnt` strictly increases for each key. This holds because:
- the node and its gateways have different addresses, so they never share a nonce even though they share the node's key;
- several gateways can't collide because each has its own address;
- addresses must be unique (§4).

### 10.4 Keys

Every key below is derived with the NIST SP 800-108 counter-mode KDF, using AES-CMAC (RFC 4493) as the PRF and producing one 128-bit block:

```
KDF(key, label, context) = AES-CMAC(key, 0x01 || label || 0x00 || context || 0x00 0x80)
```

The label is ASCII. `node` is the node's address (LE), `epoch` a 32-bit LE value.

| Key | Derived as | Held by |
|---|---|---|
| **Master key** `M[key_gen]` | random, 128 bits | the provisioning tool, and optionally gateways |
| **Chain key** `CK(0)`, the node's provisioned key | `KDF(M, "roclink node key", netId \|\| node \|\| key_gen)` | the node, as provisioned |
| **Chain key** `CK(e+1)` | `KDF(CK(e), "roclink chain", netId \|\| node \|\| key_gen \|\| e+1)` | the node and its gateway: current epoch only |
| **Frame key** `FK(e)`, the AES-CCM key | `KDF(CK(e), "roclink frame", netId \|\| node \|\| key_gen \|\| e)` | computed from `CK(e)` |
| **Session key** `SK`, the AES-CCM key in session mode | `KDF(CK(e), "roclink session", netId \|\| node \|\| key_gen \|\| join_fcnt \|\| gateway \|\| gw_fcnt)` | the node and the one gateway it joined, in RAM only ([§10.8](#108-sessions)) |

**Epochs and the ratchet (forward secrecy):**
- **The epoch comes from the node's frame counter:** `e = fcnt >> 16`. A node frame uses `FK(e)` for its own `fcnt`. A gateway frame uses `FK(e)` for the `fcnt` of the node frame that opened the reply (§10.6), so both directions of an exchange use the node's chain.
- **Advancing:** the epoch advances when the node's counter crosses a multiple of 65,536. A node can advance at any time, for example daily, by jumping its counter to the start of the next epoch, since counters may skip values. At one epoch a day, the 32-bit counter lasts about 179 years.
- **One way only:** `CK(e+1)` is computed from `CK(e)`, never the reverse. Both sides keep only the *current* chain key and erase the previous one once they've moved on. A key stolen in epoch `e` therefore reveals no frame from an earlier epoch.
- **Ratchet limits:** each epoch a receiver ratchets through costs one AES-CMAC, so the epochs one frame may move a node's key are bounded. That bounds the work a forged frame can cause, and the receiver checks the bound from the header, before any cryptography.
  - **From a node's stored chain key:** at most a configured limit, **32 epochs by default** (a month of daily epochs a running node spent out of reach), and never more than 1,024.
  - **First contact, deriving from the master:** at most 1,024 epochs from epoch 0 under open admission, or the configured limit under listed admission (below).
  - A node that is further ahead must be re-enrolled at its current epoch.
- **Key separation:** chain keys are only ever used as CMAC keys, and frame keys only as CCM keys.

**Gateways hold node keys in one of two ways:**

| | Gateway holds | It loses forward secrecy if stolen? | Enrolment |
|---|---|---|---|
| **Master mode** | `M` | **Yes:** `M` regenerates every chain | open admission: none, derived on first contact. Listed admission: the gateway derives `CK` itself for each node on its list |
| **Enrolled mode** | each node's current chain key only | **No** | the provisioning tool gives each node's `CK` to the gateway |

**Admission** (master mode): which nodes a gateway accepts without being told about them first.
- **Open**, the default: any node whose key derives from the master, so a new node can join without the gateway being updated. First contact costs up to 1,026 key derivations, so gateways should budget first contacts (see the rate-limiting guidance in SECURITY.md).
- **Listed:** only nodes on a list from provisioning. Any other source is refused before any cryptography, and every frame's cost is bounded by the configured ratchet limit. Enrolled mode always behaves this way.

**Other rules:**
- **Why per node:** stealing one node exposes only that node's keys, not the network.
- **Rotation:** provision new chain keys under `key_gen + 1`. Gateways make the new generation their *current* one and keep accepting the previous one until every node has moved over, then drop it. Frame counters and epochs may restart under a new generation.
  - A gateway holds at most two generations, and its current generation is always the one nodes are moving *to*.
  - Once a node has sent an authentic frame under the current generation, its frames under the older one are refused (§10.7 step 3). A gateway keeps counters only for a node's current generation, so this is what stops recorded old-generation frames from being replayed after the switch.
- **Provisioning and storage** are deployment requirements, covered in [SECURITY.md](SECURITY.md#deployment-requirements). Forward secrecy holds only if old chain keys are really erased from storage, not just from memory.

### 10.5 Frame counters

- **Senders:**
  - every sender keeps a 32-bit `fcnt` per key and increments it for every new secured frame, including ACKs;
  - a retransmission is the identical frame, with the same `fcnt`;
  - a node uses one counter for its key;
  - a gateway may use one counter for all nodes, since the nonce includes `dst`;
  - **a counter must never repeat under a key**, including across resets and power loss. A sender that would wrap past `0xFFFFFFFF` must stop sending secured frames until it is re-keyed.
  - **a gateway skips its counter forward at every start**, by 65,536 by default (`roclink_fcnt_skip()`). Its stored counter may have come back from a backup, and skipping is the only defence against re-using values it sent after that backup was taken: a repeated nonce under CCM reveals the XOR of two plaintexts. A gateway's counter has no epoch meaning, so skipping costs nothing; 65,536 starts use one 2³² counter. A node can't do this, because its counter's top 16 bits are its key epoch; it relies on block reservation, and must never be restored from an image or backup.
- **Persisting counters** uses block reservation, provided as a library helper:
  - the counter advances in blocks of `B` (default 256);
  - before a frame uses the first value of a new block, the sender stores the start of the *next* block in non-volatile memory;
  - after a reset, it continues from the stored value;
  - one write per `B` frames wears flash or EEPROM very little, and a power loss skips at most `B` values.
  - Keeping the counter in RTC memory over deep sleep is fine, but persistence is still required for power loss.
- **Persisting the chain key (node):** when the counter enters a new epoch, the node computes the next chain key and must store it, overwriting the old one, before sending in that epoch. If a restored counter is behind the stored key's epoch, the node skips its counter forward to that epoch's start; it can't go back.
- **Receivers (gateway side):**
  - keep the last accepted `fcnt` for each node, under the node's current key generation (see Rotation in §10.4);
  - `fcnt` greater than the last means **new**;
  - equal means **retransmission**: ACK it again, don't deliver it again;
  - lower means **replay**: drop it;
  - gateways must persist these values together with each node's chain key and epoch, so a restart doesn't reopen a replay window;
  - **clone detection:** a gateway also keeps the tag of the last accepted frame. An *authentic* frame with the same `fcnt` but a different tag means a second device holds the node's key, or the node's counter was reset. The gateway drops it and reports it as a conflict (GATEWAY_INTERFACE.md §4). A burst of replays from one node points the same way.
- **Across gateways,** the hub refuses replays (§9.1 item 3).

### 10.6 Downlink freshness

A node doesn't need to persist anything to reject replayed downlink. Every secured gateway frame (ACK or DATA) includes, in its AAD, the `fcnt` of the node frame that opened the reply (§10.3). This is the same idea LoRaWAN 1.1 uses to bind ACKs to the confirmed uplink's counter (ConfFCnt).

- **The node** verifies gateway frames using the `fcnt` of the frame it just sent. So a recorded downlink, such as "open valve", fails verification in any later window, even after the node lost power. Within one reply window, the node also requires each gateway frame's `fcnt` to be greater than the previous one from that gateway.
- **The window closes when the node stops listening.** From then until its next `LISTEN` or `ACK_REQ` frame, the node refuses every gateway frame. Otherwise a downlink recorded while the node was jammed could still be delivered later, up to the node's next frame.
- **The gateway** binds to the `fcnt` of the node's DATA frame that opened the reply. Frames sent after `MORE`, in the same reply, bind to that same value.

**Always-listening nodes.** There's no reply window, so unsolicited downlink is bound to the node's most recent uplink instead:
- **The binding** is the `fcnt` of the node's most recent secured DATA frame, with or without `LISTEN`. The gateway uses the value the hub gives it with the downlink, so any gateway can send it, including one that didn't hear that uplink (§9.1 item 2).
- **The node** verifies every gateway frame against the `fcnt` of its own most recent DATA frame. The window never closes, but it moves: once the node sends a new DATA frame, every downlink bound to an older one fails.
- **Within one binding,** the node requires each gateway's `fcnt` to be greater than the last one it accepted from *that gateway*. It tracks this per gateway in RAM. A gateway's counter only increases, so the check holds across bindings too.
- **After a reboot,** the node has no binding and accepts nothing until it has sent a DATA frame.
- **A late downlink** recorded while the node was jammed can be delivered until the node's next DATA frame, so the heartbeat interval bounds it (node rule 7).
- **If the node sent an uplink the hub never heard,** the frame fails verification, and the gateway reports it expired. It can be offered again bound to a later uplink.

### 10.7 Receiving a frame

In this order:
1. **Decode** the header (§3). `SECURED` frames must be long enough for the security header and tag. Check `netId` and addresses.
2. **Revocation and policy:** a revoked sender is refused. Then check the level (`none`, `auth`, `enc`) and the tag length against the policy for this sender; if either is too low, drop it.
3. **Suite and generation:** if the suite is unsupported or the key generation unknown, drop it. A node that has already moved to the current generation can't go back to an older one.
4. **Cheap checks before any key work (gateway):**
   - if `fcnt` is below the last accepted, drop it as a replay;
   - an epoch older than the stored chain key is a replay too;
   - a node that isn't enrolled is dropped unless admission is open and the gateway holds the master;
   - an epoch further ahead than the ratchet limits allow (§10.4) is dropped.
5. **Key:** compute the frame key for the frame's epoch, ratcheting forward from the stored chain key, or from the master on first contact. A frame in the node's current epoch needs no derivation: gateways keep that epoch's frame key. For a session-mode node, every frame except `JOIN` uses its session key instead; with no session for it, drop the frame as `no_session` (§10.8).
6. **Verify** the tag, and decrypt for `enc`. If verification fails, drop it.
7. **Same counter, different tag:** a conflict (§10.5). Drop it and report it (GATEWAY_INTERFACE.md §4).
8. **Only now update state:** the chain key and epoch (the older key is dropped), counters, last tag, duplicate tables, `DL_QUEUED`. Then deliver.

A frame that fails any step changes no state. This stops forged frames from advancing counters, ratcheting keys or filling tables.

### 10.8 Sessions

Session mode is optional, set per node on the node and at the hub. At each boot the node *joins* a gateway; both sides derive a fresh session key, and the rest of the session's frames use it with frame counters that start at 0. It's the answer to one weakness of the default mode: a node whose flash is restored from an image re-uses frame counters, which repeats CCM nonces (SECURITY.md R8). In session mode no session key is ever used twice, so that can't happen.

It costs two frames per join, and the node can't send secured data until a gateway answers. It suits mains-powered and always-listening nodes; sleepy battery nodes keep the default.

**The join.**
```
node     |== JOIN JOIN_REQ  ACK_REQ|LISTEN ==|   [listening]
gateway                                      |== JOIN JOIN_ACK ==|
both:    SK = KDF(CK(e), "roclink session", netId | node | key_gen | join_fcnt | gateway | gw_fcnt)
```

| Frame | Type, schema | Payload | Sealed with |
|---|---|---|---|
| `JOIN_REQ`, node → any gateway or its gateway | `JOIN` (2), schema 0, `ACK_REQ \| LISTEN` | `01` | `enc`, the node's frame key `FK(e)`, its persisted chain counter |
| `JOIN_ACK`, gateway → node | `JOIN` (2), schema 0, the request's `seq` | `02` \| `lifetime_s` (4, LE) \| `heartbeat_s` (2, LE) | `enc`, `FK(e)`, bound to the request's `fcnt` (§10.6) |

- **No randomness.** `join_fcnt` is the `JOIN_REQ`'s frame counter and `gw_fcnt` the `JOIN_ACK`'s. Both are already unique: a gateway refuses a node counter it has seen (§10.5), and a gateway's own counter skips forward at every start. So every join yields a new session key, and every session's counters can start at 0.
- **A restored node** sends a `JOIN_REQ` with a counter the gateway has already seen. The gateway refuses it as a replay, and the node tries again with the next values until it passes. The worst a reused join nonce can reveal is the `JOIN_REQ` payload, the constant `01`.
- **The session belongs to the gateway that answered.** The node sends to that gateway's address (not `0x0000`) for the rest of the session, and the hub sends its downlink through that gateway (§9.1 item 2). Other gateways can't open its frames and drop them as `no_session`.
- **Sealing in a session** is §10.3 with `SK` in place of `FK(e)`. Each side's counter starts at 0; node and gateway have different addresses, so their nonces differ. Session frames don't ratchet: their `fcnt` carries no epoch.
- **Freshness** works as in §10.6, on session counters: gateway frames are bound to the node's session frame they answer, or for an always-listening node to its latest session uplink. The hub's binding names the session (§9.1 item 2), so a gateway never applies it to another one.
- **Lifetime.** The gateway sets `lifetime_s` (86,400 s by default) and `heartbeat_s`. The node re-joins before the lifetime ends, moving to the next epoch first if a day has passed (SECURITY.md R5), so the chain key still ratchets daily. A session also ends when either side reboots, or when a counter would pass `0xFFFFFFFF`.
- **Lost sessions.** Sessions live in RAM only and are never persisted: that is the point. A gateway that restarts has none, and drops session frames as `no_session`. It can't answer them, so the node notices that its `ACK_REQ` frames go unanswered and re-joins (node rule 8). A `PING` with `ACK_REQ` is the cheap way to check.
- **What the node still persists:** its chain key and epoch, and the chain counter's reservation, now used by `JOIN_REQ` frames only: one write per 256 joins.
- **Forward secrecy** is unchanged: `SK` comes from `CK(e)`, so a stolen chain key and recorded joins reveal that epoch's sessions, and no earlier ones. Per-session forward secrecy needs ephemeral key agreement, which would extend this join ([§15](#15-future-work) item 1).
- **Rejected alternative: a time-based (TOTP-like) frame counter**, which would also avoid persisting counters. Clocks reset on power loss, can be set backwards by a forged time sync, collide within a time step, and drift; nonce uniqueness can't depend on a clock.

## 11. Test vectors

Network `0x42`, node `0x1234`, gateway `0xFF01`, all **unsecured**.

| # | Frame | Bytes (hex) |
|---|---|---|
| 1 | Uplink DATA, node → any gateway, seq 7, schema 1, `ACK_REQ \| LISTEN`, payload `"hi"` | `20 03 42 34 12 00 00 07 01 68 69` |
| 2 | ACK, gateway → node, seq 7, `MORE` | `24 04 42 01 FF 34 12 07 00` |
| 3 | Downlink DATA, gateway → node, seq 0x90, schema 10, `ACK_REQ \| DL_QUEUED`, payload `3C 00` | `20 09 42 01 FF 34 12 90 0A 3C 00` |
| 4 | ACK, node → gateway, seq 0x90 | `24 00 42 34 12 01 FF 90 00` |
| 5 | DATA relayed by `0xBEEF` (1 hop), node → any gateway, seq 8, schema 3, payload `AA` (reserved; v1 receivers drop it) | `20 30 42 34 12 00 00 08 03 EF BE AA` |

These vectors are checked by `test/test_frame.c`, `test/test_client.c` and `test/test_server.c`.

**Secured vectors.** Master key `00 01 02 … 0F`, key generation 0, same network and addresses. The application payload of S1 is the 13-byte BME680 example (`01 29 09 18 10 CD 8B 01 00 C0 51 02 00`).

| # | Frame | Bytes (hex) |
|---|---|---|
| K | `CK(0)`, the node key for `0x1234` (§10.4) | `E3 4F C4 12 A3 2B 76 A9 28 FE 09 D3 F1 98 95 2D` |
| K0 | `FK(0)`, frame key for epoch 0 | `E4 F3 8D 15 CD 89 18 7E 15 B7 D3 B8 F4 C1 20 31` |
| K1 | `CK(1)`, chain key for epoch 1 | `90 87 57 CA 2D BD 67 E3 F9 E0 A0 60 4C CA 2B 72` |
| K1f | `FK(1)`, frame key for epoch 1 | `3D 43 73 42 E3 FC 95 87 28 75 F1 B6 D3 EB FA 00` |
| S1 | `enc`, 8-byte tag: DATA node → any gateway, seq 7, schema 0x20, `ACK_REQ \| LISTEN`, fcnt 1 | `20 83 42 34 12 00 00 07 20 30 01 00 00 00 D8 61 5E 8A A3 76 B1 7E 23 01 D5 F2 D7 E3 52 0A 64 69 E1 7F 64` |
| S2 | `auth`, 4-byte tag: DATA node → gateway `0xFF01`, seq 8, schema 1, fcnt 2, payload `"hi"` (readable) | `20 80 42 34 12 01 FF 08 01 00 02 00 00 00 68 69 6C DB 84 CD` |
| S3 | `enc` ACK, gateway → node, seq 7, `MORE`, fcnt 0x100, bound to S1 (fcnt 1) | `24 84 42 01 FF 34 12 07 00 30 00 01 00 00 5F DA 6C DC E1 B5 8F 30` |
| S4 | `enc` downlink DATA, gateway → node, seq 0x90, schema 10, `ACK_REQ \| DL_QUEUED`, payload `3C 00`, fcnt 0x101, bound to S1 | `20 89 42 01 FF 34 12 90 0A 30 01 01 00 00 DF 10 BB 46 80 E1 C6 14 85 2B` |
| S6 | `enc`, first frame of epoch 1: DATA node → any gateway, seq 9, schema 0x20, `LISTEN`, fcnt 0x00010000, same payload as S1, under `FK(1)` | `20 82 42 34 12 00 00 09 20 30 00 00 01 00 4E F9 2E C9 36 EA 00 1C DD DA 89 34 89 A6 B0 B2 0E 9F 70 43 7F` |
| S7 | `enc` `PING` link check (§7.1): DATA node → any gateway, seq 0x0A, schema 0, `ACK_REQ \| LISTEN`, payload `01`, fcnt 3 | `20 83 42 34 12 00 00 0A 00 30 03 00 00 00 F0 3C 5C 69 CD 79 B3 6E 8B` |

These were generated by `tools/gen_vectors.py`, an independent Python implementation of §10 using OpenSSL. It also asserts the RFC 4493 (CMAC) and RFC 3610 (CCM) published examples. `test/test_secure.c` checks that the C library produces and accepts these exact bytes with every AES provider, and that every rejection case in §10.7 is enforced.

## 12. Reserved space and versioning

- **Version:** a receiver drops any frame whose `ver` ≠ 1. An incompatible header change increments `ver`.
- **Frame types:** 2 is `JOIN` (§10.8); 3–7 are reserved for control frames, such as beacons.
- **Schema 0** carries roclink control messages (§7.1). Operation `0x01` is `PING`; the rest are reserved, for example for time or configuration.
- **Broadcast** (`0xFFFF`) downlink is reserved. Securing it would need a group key.
- **Security suites** 1–3 are reserved, for example Ascon-AEAD128 (NIST SP 800-232).
- **Relays:** `RELAYED`, `HOPS` and the relay id are reserved for nodes that forward other nodes' frames toward a gateway. A relay keeps `src`, `seq`, `schema` and any security fields, and the security AAD excludes what a relay changes. The reply rules are to be defined. v1 receivers drop relayed frames.

## 13. Changes from roclink 0.1

| 0.1 | Now | Why |
|---|---|---|
| `0x0000` was *the* gateway; replies came from `0x0000` | `0x0000` means "any gateway"; gateways have addresses `0xFF00`–`0xFFFE` and reply from their own | Buildings have several gateways. Nodes can tell gateways apart and bind to one. |
| Node addresses `0x0001`–`0xFFFE` | `0x0001`–`0xFEFF` | Leaves room for the gateway range. |
| One downlink `seq` counter shared by all nodes | One per node | With a shared counter, 256 downlinks to other nodes could reuse a node's last `seq`, and its new command was silently dropped while the gateway saw an ACK. |
| `RX_WIN`, `PENDING` | `LISTEN`, `MORE` (same bits) | Names that don't assume a radio. |
| Broadcast DATA accepted by nodes | Reserved | It was never sent, and no rules existed for it. |
| Reply windows, airtime and regional limits were part of the protocol | Belong to the transport/timing layer | Keeps the wire protocol independent of hardware. |
| No security | AES-128-CCM authentication or encryption, per-node keys, frame counters, downlink freshness, receiver policy | Downlink drives actuators, and LoRa reaches outside the building. |
| One long-term key per node | Chain keys ratcheted per epoch, separate frame keys; master on gateways optional | Forward secrecy: a stolen node reveals no earlier epoch's traffic. |
| No revocation | Per-node `deny` policy, conflict detection for cloned nodes | Limits what a stolen node can do from now on. |
| Frames of at most 255 B | 255 B standard, optional jumbo frames up to 65,535 B | ESP-NOW v2 and UDP carry 1470 B frames; the length comes from the carrier, so only the limit changes. |

The header layout is unchanged, but 0.1's ACK test vector changed because gateways no longer send from `0x0000`. 0.1 and this version don't interoperate.

### 13.1 Changes in 0.3

The wire format is unchanged from 0.2, and every test vector in §11 still holds. These are rule changes, decided in the review of 0.2, and implemented in the 0.3 library and the demo hub, except session mode, which is specified only so far. `PING` is implemented on the node side (`roclink_client_build_ping()`); a gateway or hub sending one to a node is still to be built.

| 0.2 | 0.3 | Why |
|---|---|---|
| `allow-none` could be any net's policy | Per node, or network-wide only on an open net with no secured nodes; unsecured rules moved to [Annex B](#annex-b-unsecured-operation) | Network-wide `allow-none` let anyone spoofing a secured node draw out its downlink in plaintext (§10.1) |
| "A receiver policy may require 8" (not implemented) | Minimum tag length is part of the policy, 8 by default | Secure by default; 4-byte tags become an explicit choice at both ends |
| Gateways transmit only in reply windows | Always-listening node class with unsolicited downlink bound to the latest uplink | Mains-powered actuators shouldn't wait for a poll interval |
| Each gateway numbers its own downlink; node duplicates by `(src, seq)` | Downlink is numbered per node behind the gateways (§9.1); node duplicates by `seq` | A downlink re-offered through another gateway must not run twice |
| Nothing required of the system behind the gateways | §9.1 lists what any hub must do, including refusing replays across gateways | Replay protection was per gateway, so a frame could be replayed to a gateway that never heard it |
| The header byte at offset 8 was called `port` | Renamed `schema` (message schema); the byte is unchanged | It selects a payload layout and meaning, not an endpoint, and "port" clashed with UDP ports in the same documents |
| Schemas per deployment | Schemas per device type | Makers can't coordinate one schema table |
| Key rotation: "accept both generations" | The current generation is the one nodes move to; older ones are refused per node once it has moved | Gateways keep counters for one generation only; the ordering is what keeps that replay-safe |
| Schema 0 reserved, no control messages | `PING` (schema 0, operation `0x01`), the keepalive and link check (§7.1) | Always-listening nodes need a heartbeat; session-mode nodes need a cheap way to check their session |
| Frame counters persisted for every frame | Optional session mode: a `JOIN` (frame type 2) at boot gives a fresh session key (§10.8) | A node restored from a flash image re-used counters, repeating CCM nonces |
| `seq` width, schema width and gateway range open | Decided: `seq` and schema stay 8 bits, gateways stay `0xFF00`–`0xFFFE` (§3, §4, §5) | The header and every test vector stay as they are |

## 14. Open questions

1. **Uplink replies with several gateways.** Downlink is settled: the hub picks the gateway (§9.1). For replies to an uplink sent to `0x0000`, v1 uses "answer any" as configuration, so only one gateway in an area of overlapping coverage answers. An alternative is to let a node learn the gateway that answered it and address that gateway from then on (the library already supports this via `gateway_id`; session-mode nodes already do it, §10.8).
2. **Stream transport.** Should a framing for carrying roclink frames over UARTs and TCP, for example a gateway's radio on a serial port, be standardised (SLIP/COBS + CRC) or left to each deployment?

## 15. Future work

Decided in the 0.2 review to leave out of 0.3. Each would change the threat picture in [SECURITY.md](SECURITY.md); the open decisions are for when it's taken up.

1. **Sessions with ephemeral key agreement.** Not a major threat for this deployment yet: the session join (§10.8) already removes nonce reuse after a restore, and the daily ratchet limits a stolen key to one day of traffic. The design point:
   - **what it adds:** each `JOIN` frame carries a 32-byte ephemeral X25519 public key, and `SK` mixes the ECDH secret with `CK(e)`. Recorded sessions then stay secret even if the chain key is stolen later that day, and a thief who only listens can't read the next session;
   - **dependencies:** X25519, already available on every platform roclink supports (OpenSSL, PSA Crypto, ESP-IDF, rweather's Curve25519 in `roclink-crypto`), so no new library. No hash, signatures or certificates: the `JOIN` frames' existing tags authenticate the keys, and the AES-CMAC KDF absorbs the secret;
   - **the real new requirement:** a random number generator on the node. ESP32, nRF52, many STM32, Linux and Zephyr have one; AVR and SAMD21 don't. Mixing `CK(e)` into `SK` means a weak generator only loses the forward-secrecy gain, never today's protection;
   - **costs:** `JOIN_REQ` grows to 55 B and `JOIN_ACK` to 61 B, too big for LoRa SF10 in US915 and for nRF24. On an Uno each join takes about 5.4 s of X25519 (rweather's benchmark), about 0.2 s on a Due; the gateway does it only after the `JOIN_REQ` tag verifies;
   - **likely shape:** an option within session mode, for nodes with a hardware random number generator.
2. **Pairing over the air.** A PAKE such as EC-JPAKE (Thread) or SPAKE2+ (Matter), with a short code shown out of band, so nodes can be onboarded without a wired provisioning step. Open: the PAKE, how the code is shown, and how it works with enrolled-mode gateways.
3. **Signed commands for actuators.** The hub signs critical downlinks (Ed25519, 64 B), and nodes verify them with the hub's public key, so a stolen gateway or master key can't command actuators. Open:
   - which commands need it;
   - how freshness works: the signed message must include the reply binding;
   - the size cost: 64 B fits ESP-NOW and LoRa SF7/SF8 only.
4. **End-to-end payloads.** Encrypting a payload for the user's own viewer, so gateways and servers forward it without being able to read it, is developed separately as an optional extension (`roclink_e2e`), outside this spec. It would need a gateway to forward `netId`, `src`, `schema`, `fcnt`, `key_gen` and the payload unchanged (GATEWAY_INTERFACE.md).
5. **Self-description.** MySensors and Matter devices announce what they are, so a controller shows them with no code; roclink payloads need a decoder written per device type. A schema-0 operation for a node to describe its device type and schemas, plus a few standard payload conventions, would let a hub show a new device without one. Open: what the description carries, and whether it's sent once at join or on request.

## Annex A. Decoding checklist for implementers

```
if len < 9                          -> drop
ver  = b[0] >> 5;        if ver != 1           -> drop
type = (b[0] >> 2) & 7;  if type not in {0, 1, 2} -> drop   # 2 = JOIN (§10.8)
flags = b[1]; net = b[2]
src  = b[3] | b[4] << 8;  dst = b[5] | b[6] << 8
seq  = b[7]; schema = b[8]
off  = 11 if flags & 0x10 else 9;    if len < off -> drop
if flags & 0x80:                                   # SECURED
    if len < off + 5 -> drop
    sec_ctrl = b[off]; fcnt = le32(b[off+1 : off+5]); off += 5
    taglen = 8 if sec_ctrl & 0x10 else 4;  if len < off + taglen -> drop
    payload = b[off : len-taglen]; tag = b[len-taglen : len]
    -> policy, key, replay, verify/decrypt (§10.7) before trusting anything
else:
    payload = b[off : len]
if type == 1 and (payload not empty or schema != 0) -> drop   # an ACK is a header only
then apply the client rules (§8) or the server rules (§9)
```

## Annex B. Unsecured operation

Level `none` stays part of the wire format: `SECURED` is clear, and there's no frame counter and no tag. It exists so a beginner can watch plain frames on the bench, and so a node built without the security module can still take part. Everything in this annex applies only to nodes a receiver explicitly allows to send `none`.

**B.1 Opting in.**
- `allow-none` is a **per-node** policy ([§10.1](#101-levels-and-policy)) on any net that has secured nodes, so an unsecured frame claiming any other node's address is dropped as `policy`.
- **Open nets.** A net with no secured nodes at all may allow `none` network-wide. The demo hub's public net works this way: a separate net id for testing devices without keys, whose traffic is stored apart and never delivered to services (in the roclink development repository). Nothing on an open net can be trusted, since anyone can claim any address there. A node's secured traffic always uses a different net id.
- A gateway answers an unsecured frame unsecured. Because only opted-in nodes get that far, this can't expose a secured node's downlink.
- **Never** allow `none` for a node whose downlink drives anything physical ([SECURITY.md](SECURITY.md#deployment-requirements)).

**B.2 Addresses.** An unsecured node may derive its address from hardware. The library's `roclink_node_id_from_bytes()` hashes a MAC or chip UID with 32-bit FNV-1a and maps the result to `1 + (hash mod 0xFEFF)`. Collisions are possible, so assign addresses on larger deployments. Secured nodes always use assigned addresses ([§4](#4-addresses)).

**B.3 Duplicates.**
- **Gateway:** keep `(last seq, last seen)` per node. A DATA frame with the same `seq` within the dedupe window (60 s by default) is a retransmission: ACK it if asked, don't deliver it again. With an 8-bit `seq`, the window must be shorter than 256 × the node's shortest send interval.
- **Reboots:** a node that doesn't persist `seq` starts from a random one, so its first frame after a reboot has a 1-in-256 chance of being taken as a duplicate. Persist `seq` across deep sleep to avoid it.
- **Across gateways:** the hub can't refuse replays of unsecured frames; it can only remove duplicates, by `(net, src, seq)` within the same window.

**B.4 What it doesn't protect.** Anyone in radio range can read, forge, alter and replay unsecured frames, and forge ACKs for them. Gateways pass them on marked as level `none`, so whatever consumes them knows they're untrusted.
