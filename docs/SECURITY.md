# roclink security: threat model and design notes

The wire format and rules are normative in [PROTOCOL.md §10](PROTOCOL.md#10-security). This document explains what that design protects, against whom, what it deliberately doesn't cover, and what a deployment must do for the protection to hold.

**Status:** under review for 0.3. Implemented in `src/roclink_secure.c` (security, CCM, CMAC and key derivation) and `src/roclink_aes.c` (the platform AES providers), including the 0.3 rule changes (PROTOCOL.md §13.1), the forward-secure ratchet, revocation and clone detection; `PING` and session mode are specified but not built yet. The secured reference sketches are `examples/arduino/lora_secure_sender` and `lora_secure_receiver`. It builds for ESP32 and ESP32-S3 and passes its tests on the PC with every provider; it hasn't yet been run on the boards themselves. Per-session forward secrecy, over-the-air pairing and signed commands are [future work](#future-work).

## What we protect

| Asset | Property needed | Why |
|---|---|---|
| **Downlink commands** to actuators (valves, relays, locks) | Authenticity, integrity, **freshness** | A forged or replayed "open" is a physical safety and security issue. |
| **Uplink readings and events** | Authenticity, integrity; confidentiality where it matters | Forged alarms or readings mislead the system. Occupancy-style data (doors, CO₂, power) can reveal private information. |
| **ACKs** | Authenticity, binding to the acknowledged frame | A forged ACK makes a sender think a message or command was delivered when it wasn't. |
| **Past traffic** | Confidentiality after a device is stolen | Recorded frames shouldn't become readable later. |
| **Keys** | Confidentiality | Everything above depends on them. |

## Adversaries

| | Who | Can | In scope? |
|---|---|---|---|
| **A1** | Outsider in radio range: a neighbour, the street, or a car park. LoRa reaches kilometres. | Listen, record, replay, inject, modify by jamming and retransmitting | **Yes**, the main threat |
| **A2** | Device on the building LAN/Wi-Fi | Send UDP to the gateway as any `src`, sniff unencrypted LAN traffic | **Yes** |
| **A3** | Someone who steals or opens a node | Read that node's flash: its current chain key and counter | **Yes**: limited to that node, and to now and later |
| **A4** | Someone who steals or controls a gateway host (the Pi) | Everything the gateway can do; read its stored keys | **Partly**: an enrolled-mode gateway reveals no past traffic. A gateway's live powers are trusted. |
| **A5** | Jammer or flood | Deny service | **No**: radio availability can't be guaranteed by a protocol |
| **A6** | Whoever runs or breaks into the hub, MQTT broker, database or web server | Read and alter everything forwarded; replay old records; send downlink | **No**: trusted, and reached only over TLS. The hub is also a security control: it enforces replay protection across gateways and is the only source of downlink (PROTOCOL.md §9.1), so protect it at least as well as a gateway. End-to-end payloads, which would cover this, aren't part of 0.3 (PROTOCOL.md §15 item 4). |

## What the design does about each threat

| Threat | Mitigation | Where |
|---|---|---|
| Forged frames (A1, A2) | AES-128-CCM tag over the envelope and payload, under a per-node frame key | §10.3 |
| Eavesdropping on payloads (A1, A2) | `enc` level is the default | §10.1 |
| Replayed uplink (A1) | Strictly increasing 32-bit `fcnt`, last value persisted by gateways | §10.5 |
| Uplink recorded near one gateway and replayed to another that never heard it (A1) | The hub's persistent anti-replay window per node, across all gateways | §9.1 |
| Replayed downlink, e.g. a recorded "open valve" (A1) | Downlink bound to the `fcnt` of the node frame that opened the reply. Old downlink fails verification even after the node loses power. | §10.6 |
| Delayed downlink: a reply recorded while the node was jammed, delivered later (A1) | The node refuses gateway frames once its listen window closes (`roclink_sec_close_window()`), until its next frame that opens one | §10.6 |
| Forged ACK, so the sender believes delivery happened (A1) | ACKs are secured and bound to the frame they acknowledge | §10.3, §10.6 |
| Downgrade by sending unsecured frames (A1, A2) | The receiver's policy decides the minimum level and tag length; `require-enc` with 8-byte tags is the default | §10.1 |
| Drawing out a secured node's downlink in plaintext with a spoofed unsecured `LISTEN` frame, then forging its ACK (A1, A2) | On a net with secured nodes, `allow-none` exists only per node, so an unsecured frame claiming a secured node is dropped before any reply. Only an open net, with no secured nodes, may allow `none` network-wide. | §10.1, Annex B |
| Replayed downlink to an always-listening node (A1) | Unsolicited downlink is bound to the node's most recent uplink, and each gateway's counter must increase | §10.6 |
| Spoofing another node's `src` from the LAN (A2) | Keys are derived from the address, so a frame claiming `src` X must carry X's tag | §10.4 |
| One stolen node compromising the network (A3) | Per-node keys; nodes never hold the master | §10.4 |
| **A stolen node revealing recorded past traffic (A3)** | **The ratchet:** chain keys move forward one way each epoch, old keys are erased, and separate frame keys are used | §10.4 |
| **A stolen gateway revealing past traffic (A4)** | **Enrolled mode:** the gateway holds only each node's current chain key, and the master stays in the provisioning tool | §10.4 |
| **A stolen node being used afterwards (A3)** | **Revocation:** a per-node `deny` policy refuses everything from that node and sends nothing to it | §10.1 |
| **A cloned node (A3)** | **Conflict detection:** an authentic frame reusing a counter with different content is refused and reported. Replay bursts from one node are also reported. | §10.5, GATEWAY_INTERFACE.md §4 |
| Header tampering (rewriting `schema`, `dst`, flags) | The header is included in the authenticated AAD | §10.3 |
| Forged frames advancing counters, ratcheting keys or filling tables | All state changes happen only after verification | §10.7 |
| Forged frames that make a gateway do key work, e.g. a far-future counter (A1, A2) | Every check that needs no cryptography runs first. A known node's key moves at most 32 epochs per frame (configurable). Listed admission refuses unknown nodes outright. `roclink_sec_open_cost()` lets the gateway budget the rest before paying for it. | §10.4, §10.7 |
| Nonce reuse, which breaks CCM completely | Sender address + counter in the nonce, unique assigned addresses, counters persisted by block reservation | §4, §10.3, §10.5 |
| Nonce reuse after a gateway is restored from a backup (operator error) | The gateway skips its counter forward 65,536 values at every start; R8 forbids restoring older state | §10.5 |
| Nonce reuse after a node is restored from a flash image (operator error) | Default mode: R8 forbids it, and conflict detection reports it. Session mode: every boot derives a new session key from two counters that can't repeat, so session traffic can't reuse a nonce, and a replayed join is refused | §10.5, §10.8 |
| A node or gateway silently gone | `PING` heartbeats and link checks; the hub can mark a node offline after three missed heartbeats | §7.1 |

## What it doesn't protect: known limits

- **Metadata is visible.** Anyone in range can read:
  - **network and addresses:** `netId`, `src`, `dst`;
  - **header fields:** `schema` (the *kind* of data), `seq`, `fcnt`, flags;
  - **traffic shape:** frame sizes and timing.

  For example, a door sensor's frames reveal when the door is used, even when encrypted. Hiding this would need padding and cover traffic, which battery nodes can't afford.
- **Forward secrecy is per epoch, and only backwards.**
  - **Per epoch:** a stolen chain key reveals the whole current epoch, so the epoch length is the window. Advance daily ([deployment requirements](#deployment-requirements)) to keep that window to a day.
  - **Only backwards:** the ratchet is deterministic, so whoever holds `CK(e)` can follow every *later* epoch. Recovering after a compromise needs fresh key agreement, which is the deferred session design. Until then, revoke and re-key a lost node.
- **Master-mode gateways have no forward secrecy of their own.** The master regenerates every chain, so a stolen master-mode gateway reveals everything. Use enrolled mode where gateways might be stolen.
- **Erasing old keys must really erase them.** Flash and wear-levelled stores such as ESP32 NVS keep superseded data until a page is reclaimed. Without flash encryption, an old chain key may still be readable from a stolen node's flash, which defeats the ratchet. This is why flash encryption is a deployment requirement.
- **Gateways see plaintext.** The gateway service verifies and decrypts roclink frames, then forwards over TLS, so it and everything after it can read payloads.
- **A stolen node can impersonate itself until it's revoked.** Clone detection only catches a clone when the two devices' counters collide or interleave. A thief who keeps the node quiet and uses its key alone won't trigger it.
- **No availability guarantees.** Jamming, flooding and replaying the last frame (which forces a re-ACK) cost airtime and battery but can't break integrity. Forged frames still cost a gateway some work before their tag fails:
  - about 1 µs (one AES-CCM check) in a node's current epoch;
  - up to 33 key derivations for a known node: 32 ratchet steps and a frame key (about 20 µs on an x86 gateway);
  - under open admission, up to 1,026 for a node the gateway hasn't seen (about 0.6 ms on x86, tens of milliseconds on an ESP32).

  Budget expensive opens with `roclink_sec_open_cost()`, or use listed admission (deployment guidance below).
- **4-byte tags** give an attacker a 1-in-2³² chance per forgery attempt. Over a radio, attempts are slow and visible, but use 8-byte tags wherever frames allow. Receivers refuse 4-byte tags unless their policy lowers the minimum for that node or network.
- **Late downlink to always-listening nodes.** A downlink recorded while the node was jammed can be delivered until the node's next uplink. The heartbeat interval bounds that window.
- **Side channels.** ESP32 builds use the hardware AES engine. Other Arduino boards use software AES from the `roclink-crypto` extension, which is table-based. That matters only on CPUs with a data cache (Cortex-M7, for example); on AVR and Cortex-M0+/M4 parts table timing isn't a practical concern. Power analysis of a captured node is out of scope (A3).

## Trust boundary

```
 [node] ──radio──► [bridge / packet forwarder] ──► [gateway service on the Pi] ══TLS══► [MQTT broker] ──► consumers
 current CK          holds no secrets               enrolled: each node's current CK, counters
                                                    master mode: the master, counters
 [provisioning tool]  master key (offline): CK(0) per node
```

- **Bridges are untrusted plumbing.** ESP-NOW serial bridges and the LoRa packet forwarder only move bytes. They hold no keys, so compromising one gives nothing beyond what a radio attacker (A1) already has.
- **The gateway service holds keys and counters:** each node's current chain key in enrolled mode, or the master in master mode. Protect the Pi: file permissions, encryption at rest for the peer store, no shared accounts.
- **Use TLS and broker authentication** on every link after the gateway.
- **Transport security underneath** (WPA2, ESP-NOW encryption) is allowed but not relied on. Leaving ESP-NOW encryption off keeps broadcast and "any gateway" working.

## Deployment requirements

A secured roclink deployment **must** meet these. Without them, the protections above don't hold for a stolen device.

| # | Requirement | Applies to |
|---|---|---|
| R1 | **Hardware key protection.** ESP32: flash encryption and secure boot v2 in release mode, with JTAG and UART download disabled, and keys in an encrypted NVS partition. Other MCUs: an equivalent, or a secure element (e.g. ATECC608) holding the key. | every secured node and ESP32 gateway |
| R2 | **No actuators on unprotected hardware.** Nodes whose downlink drives anything physical must meet R1. AVR and RP2040, which have no flash encryption, may only sense. | actuator nodes |
| R3 | **The master key stays offline.** It lives in the provisioning tool. Gateways run in enrolled mode unless they're physically secured and the forward-secrecy limit of master mode is accepted. | gateways, provisioning |
| R4 | **Overwrite old keys in place.** Store the chain key and epoch in one record that each epoch overwrites, never as a log. The same applies to the gateway's peer store. | nodes, gateways |
| R5 | **Advance epochs at least daily.** Call `roclink_fcnt_next_epoch()` once a day, so a stolen key reveals at most a day of traffic. | nodes |
| R6 | **Have a revocation procedure.** When a node is lost or stolen, set its peer policy to `deny`, re-provision a replacement under a new address or key generation, and alert on `ROCLINK_E_CONFLICT` and replay bursts (gateways report them as described in GATEWAY_INTERFACE.md §4). | operators |
| R7 | **Unique, assigned addresses** on secured networks; no hash-derived addresses. | provisioning |
| R8 | **Never restore counter or key state from a backup or image.** A node in the default mode restored this way re-uses frame counters, which repeats CCM nonces: re-provision it instead (`roclink_eeprom_erase()` or a fresh NVS, then a new key generation or a new address). Session-mode nodes tolerate it (PROTOCOL.md §10.8). Gateways skip their counter forward at every start, which covers a backup up to 65,536 downlink frames old; restoring an older one needs the network re-keyed. | nodes, gateways, operators |

## Deployment guidance

**Key provisioning:**
- **Recommended:** provision on first boot over serial, into encrypted NVS, from a provisioning tool that holds the master and derives `CK(0)` for that node. For enrolled-mode gateways, the same tool gives the gateway each node's `CK(0)` (`roclink_sec_peer_enroll`).
- **Compiled-in** (a git-ignored `secrets.h` per node) is acceptable only for development.
- Never commit master keys, and never put the master on a node.

**What nodes persist:**
- **The counter reservation:** one write per 256 frames, plus one per boot. On AVR EEPROM at one frame a minute that's about 2,000 writes a year, well inside 100k write cycles. ESP32 NVS is wear-levelled.
- **The chain key and epoch:** one write per epoch, so one a day with R5.

**What gateways persist:** one record per node, 51 bytes of fields: last counter, epoch, chain key, the frame key derived from it, last tag, key generation, state and policy. Write it whenever `roclink_sec_open()` accepts a new frame. `sizeof(roclink_sec_peer_t)` is larger (52 bytes on common targets) and its layout depends on the compiler, so store the fields in a fixed format rather than the struct's raw bytes if the store must survive a rebuild or move to another machine.

**What the hub persists:** each node's anti-replay window (highest counter and a 64-bit bitmap per `(net, src, key_gen)`), and each node's next downlink `seq`.

**Failing fast at the gateway:**
- **Admission:** a master-mode gateway accepts any node whose key derives from the master (open admission, the default), so new nodes can join without updating it. Where the node list is known, use listed admission and `roclink_sec_peer_admit()` for each node: unknown sources are then refused in nanoseconds. Enrolled-mode gateways always work this way.
- **Ratchet limit:** keep `max_epoch_skip` at 32 unless running nodes can be out of reach longer than a month; re-enrol those that are.
- **Budget the rest:** call `roclink_sec_open_cost()` before `roclink_sec_open()`. A cost of 0 is the normal case. Allow larger costs within a budget, for example one per node per hour and a global limit per second, and drop the rest unopened. Spend most of the global budget on first contacts under open admission, since those cost the most. Budget per carrier as well: spoofed UDP on the LAN costs an attacker nothing, unlike radio airtime.

**Session mode or the default:**
- **Session mode** (PROTOCOL.md §10.8) for mains-powered and always-listening nodes. Two frames per boot buy freedom from per-frame counter persistence and from nonce reuse after a restore. Keep `lifetime_s` at a day or less, so the chain key still ratchets daily.
- **The default** for sleepy battery nodes, where a join round trip on every wake would cost more than the counter writes it saves. They must meet R8.

**Policies:**
- Keep `require-enc` as the network default.
- On a net with secured nodes, `allow-none` can only be set per node. Use it only for named bench or legacy nodes, and review the list.
- Testing without keys belongs on an *open net*: a separate net id with no secured nodes, `allow-none` network-wide, and nothing routed from it to services (the hub's public net).
- **Never** allow `none` for nodes whose downlink drives actuators.

**Rotation:** move to a new `key_gen` when a master key may be exposed, or on a schedule. Make the new generation the gateways' current one and keep the old one as the second, re-provision nodes, then retire the old generation. Never configure it the other way round: a gateway keeps counters for one generation per node, and only "current = newest" keeps old-generation frames from being replayed after a node has moved (PROTOCOL.md §10.4).

## Future work

These are future work, left out of 0.3 and tracked in [PROTOCOL.md §15](PROTOCOL.md#15-future-work). They're listed here because each would change the threat picture above.

| Item | Would add | Decisions for when it's taken up |
|---|---|---|
| **Ephemeral ECDH in the session join** (§15 item 1) | Full forward secrecy per session; recovery after a node is compromised and returned, against attackers who only listen | Needs a random number generator on the node (none on AVR or SAMD21); 55–61 B join frames (too big for LoRa SF10 in US915); whether the ratchet is still needed |
| **Pairing over the air** (§15 item 2) | Onboarding without a wired provisioning step, using a short code shown out of band (EC-JPAKE or SPAKE2+) | PAKE choice; how the code is shown; its interaction with enrolled-mode gateways |
| **Signed commands** (§15 item 3) | Actuator commands that a stolen gateway or master can't forge (Ed25519 by the hub) | Which commands; freshness via the reply binding; 64 B signatures fit ESP-NOW and LoRa SF7/SF8 only |

## Algorithm and library choices

- **AES-128-CCM**, suite 0:
  - it's what LoRaWAN-adjacent stacks, Zigbee, Thread, BLE, Matter and BTHome use;
  - there's hardware AES on ESP32, nRF52 and many STM32;
  - the standard allows 4- and 8-byte tags, which keeps frames small for LoRa and nRF24.
- **AES-CMAC** for all key derivation (NIST SP 800-108 / RFC 4493): node keys, the ratchet and frame keys. It needs only the same AES primitive, and only the encrypt direction.
- **No randomness needed on nodes.** Nonces come from counters. Only the provisioning tool needs a cryptographically secure RNG, to make the master key.
- **Crypto backends:** roclink implements CCM and CMAC itself, over one primitive, AES-128 encryption of a block, supplied by the platform (README.md, "The library"):
  - **ESP32:** the hardware AES engine, through ESP-IDF's driver (Arduino-ESP32 2.x and 3.x);
  - **Zephyr:** PSA Crypto;
  - **Linux gateways and host tests:** OpenSSL, chosen explicitly;
  - **every other Arduino board, AVR included:** the `roclink-crypto` extension library, using rweather's `Crypto` library;
  - **anything else:** the application's own block function.

  CCM and CMAC are checked against RFC 3610, RFC 4493 and the vectors in PROTOCOL.md §11, which an independent Python implementation generates with OpenSSL. The same tests run against every AES provider: OpenSSL, PSA Crypto, and rweather's `AES128` and `AESTiny128`. A missing or wrong provider is caught at start-up by a known-answer check. 0.2's vendored TinyCrypt and its mbedTLS backends are gone.
- **Footprint** (code, `-Os`):

  | | x86-64 | AVR (ATmega328P) |
  |---|---|---|
  | `roclink_secure` | 7.0 KB | 7.4 KB |
  | CCM + CMAC | 3.0 KB | 2.4 KB |
  | AES | OpenSSL | about 1 KB (`AESTiny128` and the provider) |

  The whole secured Uno example (roclink, security, AES, EEPROM persistence and the LoRa driver) is 17 KB of flash and 1,035 bytes of static RAM, out of 32 KB and 2 KB. On ESP32 the AES is the hardware engine, already part of ESP-IDF. Level `auth` no longer copies the payload into a stack buffer, so `seal()` and `open()` use a few hundred bytes of stack.
- **Future suites:** Ascon-AEAD128 (NIST SP 800-232) is the candidate for suite 1, aimed at very small devices.
