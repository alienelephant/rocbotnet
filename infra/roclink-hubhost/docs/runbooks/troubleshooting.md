# Troubleshooting

Start with:

```bash
roclink status          # which service is down
roclink logs <service>  # why
```

## A service won't start

`roclink status` shows it `failed` or restarting.

- `roclink logs <service>` shows the container's own error: a bad config line, a database it can't open, a port in use.
- Quadlet couldn't make the unit (a typo in a `.container` file): the unit doesn't exist at all. Check with:

  ```bash
  sudo -u roclink /usr/libexec/podman/quadlet -user -dryrun
  ```

- The image is missing (pruned too eagerly): run the service's role again (`--tags mqtt`, `apps`, `web`, `beszel`), which pulls or builds it.
- systemd gave up after too many restarts: fix the cause, then `sudo -u roclink XDG_RUNTIME_DIR=/run/user/1500 systemctl --user reset-failed` and `roclink start <service>`.

## The gateways' data doesn't arrive

1. `roclink watch 'roclink/in/#'`: does anything come in? If yes, the broker's fine; go to `roclink logs roclink-hub`.
2. On the Pi: `journalctl -u roclink-gw -n 30`.
   - `connection refused`: the broker is down, or the firewall.
   - `not authorised`: the password. `roclink-gw`'s differs between the two vaults ([mqtt-users.md](mqtt-users.md)).
   - `certificate verify failed`: the Pi's `/etc/roclink-gw/mqtt-ca.crt` isn't the CA that signed the hub's certificate. That happens after the CA is replaced; run the gateway playbook's `--tags gw` again.
   - `host name verification failed`: `mqtt_host` isn't a name the hub's certificate has (`192.168.8.134` or `roclink-hub`).
3. On the hub: `roclink logs mosquitto` shows each connection, and why one was refused.
4. From the Pi, can it reach the port? `nc -vz 192.168.8.134 8883`. If not, see "Firewall". `1883` is closed to the LAN on purpose: it's plain MQTT, for the hub's own services only.

## The scoreboard or the admin pages don't load

| Symptom | Likely cause |
|---|---|
| connection refused on `:80` | Caddy is down: `roclink logs caddy` |
| `502 Bad Gateway` | Caddy's up, the service behind it isn't: `roclink status` |
| the browser shows a connection or "bad certificate" error on `https://` | no client certificate installed, or not chosen, or not in `admin_clients` ([admin-certs.md](admin-certs.md)) |
| "not secure" / "unknown issuer" on `https://` | the admin CA isn't trusted in that browser |
| works from the hub (`curl http://127.0.0.1:8090/`), not from elsewhere | the firewall, or the client's not on `192.168.8.0/24` |

## Firewall

```bash
sudo nft list table inet roclink                  # the rules
sudo journalctl -k -g nft-drop -n 20              # what was dropped (rate-limited)
sudo fail2ban-client status sshd                  # banned addresses
sudo fail2ban-client set sshd unbanip 192.168.8.x
```

A rule change goes in `roles/harden/templates/nftables.conf.j2`, then `--tags harden`. Changes made with `nft` by hand last until the next reload.

## Disk full

```bash
df -h / /srv /var
sudo du -sh /srv/roclink/data/* /var/backups/roclink /var/log/journal /home/roclink/.local/share/containers
```

The usual causes: old images (`roclink podman image prune -a`), the hub's database growing (it keeps uplinks for good), the journal (capped at 1 GB).

## Time is wrong

```bash
chronyc tracking         # this box: "Leap status: Normal", a small offset
chronyc clients          # the gateways asking it
```

On a Pi: `timedatectl timesync-status` should show server `192.168.8.134`.

## The playbook can't connect

- `Permission denied (publickey)`: the wrong key, or `rocadmin`'s `authorized_keys` changed. `ssh -v -i ~/.ssh/id_ed25519_rocadmin rocadmin@192.168.8.134` says which.
- It hangs: a stale control connection. `ssh -O exit -o ControlPath=/tmp/.rochub rocadmin@192.168.8.134`.
- Banned by fail2ban after failed attempts: wait an hour, or from the console `fail2ban-client set sshd unbanip <your address>`.
- None of that: [lost-access.md](lost-access.md).
