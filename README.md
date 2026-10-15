# roclink: wire protocol v1 (RFC for 0.3)

roclink is a small envelope for messages between sensor and actuator boards (*nodes*) and the *gateways* that receive them, over any radio: LoRa, ESP-NOW, Wi-Fi/UDP and others. Frames are authenticated and encrypted by default.

This repository holds the RFC for review, the normative spec it summarises, and a minimal reference implementation with tests and Arduino examples.

## Documents

| | |
|---|---|
| [docs/RFC_DRAFT.md](docs/RFC_DRAFT.md) | **Start here.** The RFC: goals, scope, frame format, flows, security, open questions |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | The normative wire protocol between nodes and gateways, with test vectors |
| [docs/SECURITY.md](docs/SECURITY.md) | Threat model and deployment requirements |

How gateways hand data to a hub is a separate draft, and isn't part of this RFC.

## The library

Five files in `src/`, C99, with no heap, no clock and no dependencies:

| File | What's in it | Needed for |
|---|---|---|
| `roclink.h`, `roclink.c` | The frame format, and the node and gateway sides | everything |
| `roclink_secure.h`, `roclink_secure.c` | Security: seal and open, keys and the daily ratchet, frame counters, replay and clone detection, with roclink's own AES-CCM and AES-CMAC | secured frames |
| `roclink_aes.c` | The AES block from the platform: ESP32 hardware AES, PSA Crypto (Zephyr), OpenSSL (Linux) | secured frames |

A node or gateway that only sends unsecured frames needs just `roclink.h` and `roclink.c`.

Security needs one AES block function. On ESP32 and Zephyr it's chosen automatically; on Linux, build with `-D ROCLINK_CRYPTO_OPENSSL` and link `-lcrypto`. Other Arduino boards (AVR, RP2040, nRF52, …) get it from the separate `roclink-crypto` library, or from your own `roclink_aes128_encrypt()` with `-D ROCLINK_CRYPTO_CUSTOM`.

```c
#include "roclink.h"

roclink_client_t node;
roclink_client_config_t cfg = {0x42, 0x1234, ROCLINK_ADDR_ANY_GATEWAY};
roclink_client_init(&node, &cfg, random_byte());

uint8_t frame[ROCLINK_FRAME_MAX_LEN];
int n = roclink_client_build_data(&node, /* schema */ 1, data, len, ROCLINK_SEND_ACK,
                                  frame, sizeof frame);
radio_send(frame, n);  /* your radio */
```

## Build and test

```bash
make test           # frame, node, gateway, exchange and security tests (OpenSSL's AES)
make test-backends  # the security tests again with PSA Crypto
make test-jumbo     # everything with 1470-byte frames
make sanitize       # under ASan and UBSan
```

`test/vectors_secure.h` comes from `tools/gen_vectors.py`, an independent Python implementation of the security spec (`make vectors` regenerates it). `tools/provision.py` makes each node's key from a network master key.

## Arduino examples

| Sketch | Board | Shows |
|---|---|---|
| `examples/arduino/lora_secure_sender` | Heltec WiFi LoRa 32 V2 | a secured node: sealing, ACK and retry, counters and keys kept in NVS, a daily ratchet |
| `examples/arduino/lora_secure_receiver` | a second Heltec V2 | a secured gateway in enrolled mode (no master key), cost-checked opens, sealed ACKs |
| `espnow_*`, `lora_*`, `wifi_udp_*` | ESP32 boards | stock sketches with their data wrapped in roclink, unsecured, on the open test net `0xF0` |

The secured sketches use the published demo keys unless a sketch folder has a `secrets.h` (git-ignored) with keys from `tools/provision.py`.

Install this folder as an Arduino library (it has `library.properties`), or use PlatformIO: `pio run -e lora_secure_sender -t upload`. `pio test -e native` runs the tests on the PC, and `pio test -e test_wroom` on a board.

## License

MIT, see [LICENSE](LICENSE).
