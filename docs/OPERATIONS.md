# Operating the gateway

How to watch a running gateway, test it, and find out what's wrong. The examples use the first gateway, `rfgateyway1` at `192.168.8.237`, gateway id `0xFF01`.

The Pi is on the LAN by Ethernet at `192.168.8.237`, and runs the `roclink-nodes` Wi-Fi access point for nodes, where it is `10.42.0.1`. Wi-Fi nodes send to `10.42.0.1:3333`. From the LAN, only the MQTT broker and SSH are meant to be used; `test_node.py` works from either side.

## Passwords

Every MQTT client logs in. The passwords are in the ansible vault, on the machine you run ansible from:

```bash
cd ~/Documents/code/ai_slop/roclink-gateway
ansible-vault view group_vars/all/vault.yml
```

The node access point's passphrase is there too, as `vault_node_ap_psk` (SSID `roclink-nodes`).

| User | Can |
|---|---|
| `admin` | read and write everything: use it for watching and debugging |
| `roclink-hub` | read `roclink/in/#`; write `roclink/out/#`, `roclink/svc/#`, `roclink/pub/#`, `roclink/ops/#` |
| `roclink-gw` | write `roclink/in/#`; read `roclink/out/#` (the gateway daemon) |
| `roclink-ops` | read `roclink/ops/#` |
| any other user | a service: read `roclink/svc/<user>/#`, write `roclink/svc/<user>/down` |

## Watching MQTT

On the Pi:

```bash
mosquitto_sub -h localhost -u admin -P 'PASSWORD' -t 'roclink/#' -v
```

- `-t 'roclink/#'` subscribes to every topic under `roclink/`. `-v` prints each message's topic before it.
- Keep the password on one line inside the quotes. A line break inside them becomes part of the password, and the broker refuses the login.
- Messages aren't retained: you see only what's published while you're subscribed.

From another machine on the LAN, use the same command with `-h 192.168.8.237`. It needs the mosquitto clients: `sudo pacman -S mosquitto` on Arch, `sudo apt install mosquitto-clients` on Debian. For a GUI, MQTT Explorer can connect to `192.168.8.237:1883` as `admin`.

To pretty-print the JSON (`sudo apt install jq` first):

```bash
mosquitto_sub -h localhost -u admin -P 'PASSWORD' -t 'roclink/in/#' | jq .
```

### Topics

`<net>` is two hex digits and `<gw>` four, in lowercase: `roclink/in/42/ff01/up`.

| Topic | Who publishes | What |
|---|---|---|
| `roclink/in/<net>/<gw>/up` | gateway | an accepted uplink, decrypted (GATEWAY_INTERFACE.md §3) |
| `roclink/in/<net>/<gw>/rejected` | gateway | a dropped frame: `replay`, `auth`, `policy`, … (§4) |
| `roclink/in/<net>/<gw>/down_status` | gateway | what became of a downlink: `acked`, `sent` or `expired` (§5) |
| `roclink/out/<net>/<gw>/down` | hub | a downlink for the gateway to send (§5) |

To watch just one kind, subscribe to it, for example `-t 'roclink/in/+/+/up'` for uplinks, or `-t 'roclink/in/+/+/rejected'` for drops.

An uplink record looks like this. `payload` is hex: this one is the text "hello from the test node".

```json
{"v":1,"gateway":"0xFF01","time":1792096182766,"net":66,"src":"0x1234","dst":0,"seq":1,
 "schema":1,"flags":1,"sec":"enc","fcnt":1,"key_gen":0,
 "payload":"68656c6c6f2066726f6d207468652074657374206e6f6465","radio":{"transport":"udp"}}
```

## Watching the logs

On the Pi (no sudo needed: `botmaker` is in the `adm` group):

```bash
journalctl -u roclink-gw -f                    # the gateway daemon
journalctl -u lora_pkt_fwd -f                  # the LoRa radio (packet forwarder)
journalctl -u roclink-gw -u lora_pkt_fwd -f    # both, interleaved
journalctl -u roclink-gw --since "-10min"      # the last 10 minutes
```

The service names are `roclink-gw`, `lora_pkt_fwd` and `mosquitto`. `journalctl -u` and `-t` with a misspelled name show nothing and don't warn.

A healthy daemon logs one line per uplink:

```
[info] up 42/1234 seq 194 schema 1 [enc] 16 bytes via lora
```

Drops aren't logged at `info`, only published on `.../rejected`. To log them too, add `-v` to `ExecStart` in `/etc/systemd/system/roclink-gw.service`; ansible puts it back on the next run.

```bash
systemctl status roclink-gw lora_pkt_fwd mosquitto
sudo systemctl restart roclink-gw
```

## Sending test traffic

`tools/test_node.py` acts as a Wi-Fi node: it sends one frame over UDP, checks the gateway's ACK, and receives and ACKs any downlink queued for it. It needs Python's `cryptography` package and `../roclink-rfc` next to this repo.

```bash
cd ~/Documents/code/ai_slop/roclink-gateway
/usr/bin/python3 tools/test_node.py --gateway 192.168.8.237          # secured, net 0x42
/usr/bin/python3 tools/test_node.py --gateway 192.168.8.237 --open   # unsecured, net 0xF0
```

It uses node address `7E57` by default (`--node HEX` to change it), and keeps its frame counter in `~/.roclink-test-node-<net>-<node>`.

### Queuing a downlink by hand

As the hub would, on the Pi:

```bash
EXP=$(( $(date +%s%3N) + 60000 ))   # gives up after a minute
mosquitto_pub -h localhost -u roclink-hub -P 'HUB_PASSWORD' -q 1 -t roclink/out/42/ff01/down \
  -m "{\"v\":1,\"id\":\"t1\",\"node\":\"0x7E57\",\"schema\":10,\"seq\":77,\"ack\":true,\"payload\":\"0102\",\"send\":\"window\",\"binding\":null,\"expires\":$EXP}"
```

Then run the test node with the same address. It receives the downlink after the ACK, and `roclink/in/42/ff01/down_status` reports `acked`.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `journalctl` or `mosquitto_sub` shows nothing at all | A misspelled name (`rocklink` instead of `roclink`), or a topic that matches nothing. Check with `-t '#'`. |
| `Connection error: Not authorised` | Wrong password, or a line break inside the quotes. |
| `test_node.py`: `FAIL: no ACK within 3 s` | Look on `.../rejected`. **`replay`**: another device with the same node address has sent a higher frame counter, so give each node its own address. **`auth`**: the node's key doesn't match the gateway's master key. No report at all: the frame didn't arrive, so check the address and port (UDP 3333), and that `roclink-gw` is running. |
| `ModuleNotFoundError: No module named 'cryptography'` | Your `python` is a virtualenv or another interpreter. Use `/usr/bin/python3`, or `python -m pip install cryptography`. |
| `lora_pkt_fwd`: `chip version is 0x00` | The SX1302 isn't answering on SPI: the HAT is unseated or unpowered. Power off and reseat it. |
| `lora_pkt_fwd`: `Failed to set SX1250_0 in STANDBY_RC mode` | The SX1302 wasn't reset. This Waveshare HAT resets on GPIO17 (`sx1302_pins` in `group_vars/all/main.yml`). |
| LoRa node sends, but nothing arrives | Radio settings differ: the node must use 915.0 MHz, SF9, 125 kHz, sync word 0x12 (RadioLib's defaults in roclink's sketches). Check `journalctl -u lora_pkt_fwd` for received packets with a bad CRC. |
| A node's frames are all refused after re-flashing it | Its frame counter started again from a lower value: `replay`. Give it a new address, or remove its record by stopping `roclink-gw` and deleting `/var/lib/roclink-gw/net-42.state`. That forgets every node's counter, so do it on a test network only. |
