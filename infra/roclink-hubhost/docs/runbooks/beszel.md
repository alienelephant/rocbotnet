# Beszel

Beszel's hub runs on the hub host, at `https://192.168.8.134/` (behind a client certificate: [admin-certs.md](admin-certs.md)). It connects to an agent on each machine over SSH, with its own key. The agents let in only that key, which the hub's playbook saves as `beszel-hub.pub` (committed; the gateway playbook reads it).

| Agent | Where | Address to give Beszel |
|---|---|---|
| this host's | a container of roclink's, `127.0.0.1:45876`; sees the host and roclink's containers | `127.0.0.1`, port `45876` |
| each gateway's | a systemd service, `beszel-agent`, on `:45876` | the Pi's address, e.g. `192.168.8.237`, port `45876` |

## The first time

1. Open `https://192.168.8.134/` and create the admin account (the first visitor makes it; the client certificate is what keeps others from being first). Put its password in your password manager.
2. **Add System** → name `roclink-hub`, host `127.0.0.1`, port `45876`. Ignore the install command it offers: the agent's already running. It turns green within a minute.

## The gateways

The gateway playbook installs the agent with the hub's key:

```bash
cd rfgateways/roclink-gateway
ansible-playbook site.yml --tags beszel
```

Then in Beszel, **Add System** → name `rfgateway1`, host `192.168.8.237`, port `45876`.

A new gateway: add it to the gateway's `inventory.ini`, run its playbook as usual (that includes the agent), and add it in Beszel.

## Alerts

Set them per system in Beszel (the bell): CPU, memory, disk, temperature, and "status" for a system that stops answering. Notifications go by e-mail (SMTP, in Beszel's settings) or any Shoutrrr URL (ntfy, Gotify, Telegram, ...).

## If the hub's key changes

It changes only if `/srv/roclink/data/beszel/id_ed25519` is lost (a reinstall without restoring backups). The next `--tags beszel` saves the new one in `beszel-hub.pub`; commit it, and run the gateway playbook's `--tags beszel`, or the gateways' agents refuse the hub.

## Upgrading

`beszel_image` and `beszel_agent_image` in the hub's `group_vars/all/main.yml`, and `beszel_agent_version` in the gateway's, should stay on the same version ([updates.md](updates.md)).
