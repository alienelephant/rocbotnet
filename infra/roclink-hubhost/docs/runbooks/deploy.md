# Deploying the hub and the game

The images are built on the box from a commit of this repository: `hub/roclink-hub` and `hub/roclink-scavhunt` at `HEAD`. Uncommitted changes aren't deployed (the playbook lists them).

## A new version

```bash
git commit ...                                   # the change, in hub/
cd infra/roclink-hubhost
ansible-playbook site.yml --tags apps
```

Each image is tagged with its directory's git tree hash (`localhost/roclink-hub:3f9a1c2b7d4e`), so an app is rebuilt and restarted only if its own directory changed. The playbook then checks the hub's API and the scoreboard answer.

A deploy restarts the hub. The broker keeps its QoS 1 queue meanwhile, so no uplinks are lost; the scoreboard is down for a few seconds.

## A config change

The hub's config is `roles/apps/templates/hub.toml.j2` (device types, decoders, routes), the game's `roles/apps/templates/game.toml.j2` (boxes, points, time limit). Edit, commit if you like, and run `--tags apps`: only what changed restarts.

A new `[[route]]` is a new service, which also needs a broker user: [mqtt-users.md](mqtt-users.md).

## A specific commit, or rolling back

```bash
ansible-playbook site.yml --tags apps -e src_ref=a1b2c3d     # any commit, branch or tag
```

The image for an old commit is usually still on the box, so rolling back takes seconds. Then deploy `HEAD` again when it's fixed.

## Old images

They stay until you remove them. To see and prune them:

```bash
roclink podman images localhost/roclink-hub
roclink podman image rm localhost/roclink-hub:<old tag>
roclink podman image prune          # untagged layers
```

## A database schema change

The hub and the game create and migrate their own tables at start. Before deploying a change that does, take a backup (`roclink backup`); rolling back the code doesn't roll back the database ([backup-restore.md](backup-restore.md)).
