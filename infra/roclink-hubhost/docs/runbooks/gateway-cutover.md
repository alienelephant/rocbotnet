# Moving the gateways to the hub's broker

Once, when the hub is first set up. Before this, each gateway Pi ran its own mosquitto and the hub (on the Mac) connected to it. After it, the gateways publish to the hub's broker, and the Pis run no broker.

The hub's broker has the same users and passwords as the Pi's had (they were copied into the hub's vault), so nothing needs new credentials.

## 1. Stop the Mac's hub

Two hubs with the same client id (`roclink-hub`) on one broker throw each other off. On the Mac:

```bash
roclink stop
```

## 2. Check the hub's broker

On the hub, `roclink status` shows `mosquitto`, `roclink-hub` and `scavhunt` running, and:

```bash
roclink watch       # leave it open
```

## 3. Point the gateways at it

`rfgateways/roclink-gateway/group_vars/all/main.yml` has `mqtt_host: 192.168.8.134` and `mqtt_local_broker: false`. Run the gateway playbook:

```bash
cd rfgateways/roclink-gateway
ansible-playbook site.yml --tags gw,mqtt,base
```

This rebuilds `roclink-gw` (it needs the version with `mqtt_ca_file`), points it at the hub's broker over TLS on `:8883` with the hub's CA (`infra/roclink-hubhost/hub-ca.crt`), and restarts it. It also stops and disables the Pi's mosquitto, and sets the Pi's time source to the hub.

## 4. Check

- The `roclink watch` from step 2 shows traffic on `roclink/in/42/ff01/...` when a node sends (or `python3 tools/test_node.py --gateway 192.168.8.237` in `rfgateways/roclink-gateway`).
- On the Pi, `journalctl -u roclink-gw -n 20` says `connected to mqtt 192.168.8.134:8883 (tls)`.
- On the hub, `roclink logs roclink-hub` shows the uplinks being stored, and the scoreboard at `http://192.168.8.134/` shows box data.

## Going back

Set `mqtt_local_broker: true`, `mqtt_host: localhost`, `mqtt_port: 1883` and `mqtt_ca_file: ""` in the gateway's `group_vars/all/main.yml`, run `ansible-playbook site.yml --tags gw,mqtt` there, and point the Mac's hub back at the Pi (it still is: `infra/roclink-bench/etc/hub.toml.in`). Stop the hub host's `roclink-hub` (`roclink stop roclink-hub`) before starting the Mac's.

## The Mac's bench

`infra/roclink-bench` still connects to the Pi's broker (`192.168.8.237`). To use the bench against the hub's broker instead, change `host` in `etc/hub.toml.in` and `etc/game.toml` to `192.168.8.134` and run `setup.sh --configs`; but don't run both hubs at once.
