# roclink-bench

The running roclink hub and scavenger hunt on the bench Mac, and the scripts that keep the rocbotnet workspace together. [WORKSPACE.md](WORKSPACE.md) is the map of the whole system (linked as `~/rocbotnet/README.md`).

| File | |
|---|---|
| `setup.sh` | Installs or updates the hub and the game from `../../hub` (editable), writes `~/roclink/etc` from `etc/` with the secrets from the gateway's vault, and links `roclink` into `~/bin` |
| `roclink` | `start`, `stop`, `restart`, `status`, `logs`, `watch` (the broker, as admin), and `scavhunt …` (the game's operator commands) |
| `etc/hub.toml.in` | The hub's config: the Pi's broker, the boxes' device types and decoders, the route to the game. `@HUB_PASSWORD@` is filled in from the vault |
| `etc/game.toml` | The game: its boxes (the real ones: the Heltec's Tower, the XIAO's Workshop), scoring, the scoreboard |
| `workspace.sh` | `status`, `pull`, `push`, `clone` across every repository in the workspace |
| `rocbotnet.code-workspace` | VS Code: every repository as a folder (`code rocbotnet.code-workspace`); PlatformIO finds the firmware projects |

## The running bench

```
~/roclink/
  etc/      hub.toml, game.toml, MQTT password files (mode 700; written by setup.sh)
  venv/     hub, scavhunt: Python 3.12, editable installs of ../../hub/*
  data/     hub.db, public.db, game.db, the hub's state files
  log/      hub.log, scavhunt.log
  run/      pid files
```

`setup.sh` keeps an existing `hub.toml` and `game.toml`, so a change made on the Mac isn't overwritten: copy it back into `etc/` here to keep it. `./setup.sh --configs` writes them again from `etc/` (keeping the old ones as `.bak`). Restart after either: `roclink restart`.

The hub and game run under `caffeinate -i`, which keeps the Mac awake while they run, but not with its lid closed.

## License

MIT, see [LICENSE](LICENSE).
