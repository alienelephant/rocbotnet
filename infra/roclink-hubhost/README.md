# roclink hub host

Ansible for the hub: a Debian 13 box on the LAN (`192.168.8.134`) that runs the central MQTT broker, the hub, the game, a web front and Beszel, as rootless podman containers.

```
 gateways (Pi) ──MQTT/TLS :8883─►  mosquitto ──► roclink-hub ──► scavhunt
                                                 │                │
 browsers ──HTTP :80──────────►  caddy ──────────┴── /public/     └── / (scoreboard)
 admins ──HTTPS :8443, :9443 ─►  caddy (client certificate) ──► beszel, the hub's API
 gateways' beszel agents ◄──SSH :45876── beszel
```

| Path | What it is |
|---|---|
| `bootstrap.yml` | Once, on a fresh install: makes the `rocadmin` user that everything else logs in as |
| `site.yml` | Everything else, in tagged roles |
| `roles/base` | Updates, packages, chrony (also serving the LAN), journald, unattended security upgrades |
| `roles/harden` | Users, sshd (keys only), nftables, fail2ban, sysctl, auditd, unneeded packages removed |
| `roles/podman` | The `roclink` user whose containers run the services, and the `roclink` admin command |
| `roles/mqtt` | Mosquitto, with the users and ACL that used to be on the gateway |
| `roles/apps` | The hub and the game, built from this repository into images on the box |
| `roles/pki`, `roles/web` | The admin CA and its certificates (made on this machine), and Caddy |
| `roles/beszel` | The Beszel hub, and an agent for this host |
| `roles/backup` | Nightly database backups on the box, pruned |
| `docs/` | Administering it |

## Running it

Needs `ansible` (with `ansible.posix`), the key `~/.ssh/id_ed25519_rocadmin`, and `.vault-pass` (git-ignored: copy it by hand, as for the gateway).

```bash
ansible-playbook site.yml                  # everything
ansible-playbook site.yml --tags apps      # deploy the hub and the game from HEAD
ansible-playbook site.yml --tags mqtt      # broker users and ACL
```

Secrets are in `group_vars/all/vault.yml`, settings in `group_vars/all/main.yml`. The admin certificates are in `pki/`, which is git-ignored: keep a copy of it with `.vault-pass`.
