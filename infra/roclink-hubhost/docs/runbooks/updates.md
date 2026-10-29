# Updates

## Debian

Security updates install themselves every day (unattended-upgrades, Debian's security archive only). They never reboot the box. When one needs a reboot (a new kernel, or a package that asks), the login message says so, and so does:

```bash
reboot-required
```

Everything else, all packages, updates when you run the playbook:

```bash
ansible-playbook site.yml --tags base
```

That reboots the box if needed, and waits for it to come back. Not during a game: the services are down for a minute or two. To update without rebooting:

```bash
ansible-playbook site.yml --tags base -e allow_reboot=false
```

A reboot by hand is just `sudo reboot`: the services start on their own (roclink's systemd starts at boot, `loginctl enable-linger`).

What unattended-upgrades did: `/var/log/unattended-upgrades/`.

## The container images

They're pinned in `group_vars/all/main.yml` (`mosquitto_image`, `caddy_image`, `beszel_image`, `beszel_agent_image`, `python_image`), so nothing changes without you. To upgrade one:

1. Read its release notes (Mosquitto, Caddy, Beszel on GitHub).
2. Change the tag, and run that role:

   | Image | Tag |
   |---|---|
   | `mosquitto_image` | `--tags mqtt` |
   | `caddy_image` | `--tags web` |
   | `beszel_image`, `beszel_agent_image` | `--tags beszel` (also `beszel_agent_version` in the gateway's vars, then its `--tags beszel`) |
   | `python_image` | `--tags apps` (it isn't rebuilt unless the source changed: remove the images first, `roclink podman image rm localhost/roclink-hub:<tag> localhost/scavhunt:<tag>`) |

3. The role pulls the new image, restarts the service, and checks it.

The old image stays on the box; to roll back, put the old tag back and run the role again. `roclink podman image prune -a` removes the images nothing uses.

## Debian 14

When Debian 14 is out, test the playbook on a spare box or VM first. Then: back up and copy the backups off the box ([backup-restore.md](backup-restore.md)), change the suite in `/etc/apt/sources.list*`, `apt full-upgrade`, reboot, and run `ansible-playbook site.yml`. Or reinstall from scratch ([install.md](install.md)) and restore the databases, which is often less work.
