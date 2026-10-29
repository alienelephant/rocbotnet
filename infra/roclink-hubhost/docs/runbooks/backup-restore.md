# Backups and restoring

`roclink-backup.timer` runs `/usr/local/sbin/roclink-backup` every night at about 03:30, as roclink. `roclink backup` runs it now.

## What's in one

`/var/backups/roclink/daily/<date>/`:

| File | From |
|---|---|
| `hub__hub.db.zst` | the hub: every uplink, node, rejection, the downlink queue's history |
| `scavhunt__game.db.zst` | the game: runs, teams, scores |
| `beszel__data.db.zst`, `beszel__auxiliary.db.zst` | Beszel: its account, systems, alerts, history |
| `beszel__id_ed25519` | Beszel's SSH key, which the agents trust |

`__` stands for `/` under `/srv/roclink/data/`. Every `*.db` there is included, so a new service's database is too. Not included: `public.db` (a day of throwaway test traffic), the broker's queue, and the configs and secrets, which are in git and the vault.

Each database is copied with SQLite's online backup, so it's consistent even while the service writes, then checked (`PRAGMA integrity_check`) and compressed. A failed check fails the backup, and the previous days' stay.

## Pruning

The newest 14 daily ones are kept, and on Sundays that day's is also kept in `weekly/` (hard-linked, so it costs no more space), the newest 8 of those. Older ones are deleted by the backup itself. To change it: `backup_keep_daily` and `backup_keep_weekly` in `group_vars/all/main.yml`, then `--tags backup`.

## Checking

```bash
systemctl list-timers roclink-backup.timer        # when it last ran, and next
journalctl -u roclink-backup -n 30                # what it said
ls -l /var/backups/roclink/daily/
```

## Restoring a database

The hub's, from 2026-10-25, for example. Stop what uses it, put the file in place as roclink, start it again:

```bash
roclink stop scavhunt roclink-hub
sudo -u roclink sh -c '
  cd /srv/roclink/data/hub &&
  mkdir -p before-restore && mv hub.db* before-restore/ &&
  zstd -d /var/backups/roclink/daily/2026-10-25/hub__hub.db.zst -o hub.db'
roclink start roclink-hub scavhunt
roclink logs roclink-hub
```

The same for the game (`scavhunt__game.db.zst` into `/srv/roclink/data/scavhunt/game.db`, stopping `scavhunt`), and Beszel (`beszel__*.db.zst` into `/srv/roclink/data/beszel/`, stopping `beszel`; also `beszel__id_ed25519` as `id_ed25519`, mode 600).

`mv hub.db*` moves the `-wal` and `-shm` files too: an old WAL next to a restored database would corrupt it. Delete `before-restore/` once you're sure.

## Copying one off the box

The backups protect against a bad deploy or a corrupted database, not against the disk dying. Before anything risky, or now and then:

```bash
rsync -a rocadmin@192.168.8.134:/var/backups/roclink/daily/$(date +%F)/ ~/roclink-backups/$(date +%F)/ --rsync-path='sudo rsync'
```
