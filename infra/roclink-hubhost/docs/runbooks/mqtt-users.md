# MQTT users

The broker's users are `mqtt_users` in `group_vars/all/main.yml`, with their passwords in the vault. The ACL is `roles/mqtt/templates/roclink.acl.j2`.

| User | Who | May |
|---|---|---|
| `roclink-gw` | every gateway | write `roclink/in/#`, read `roclink/out/#` |
| `roclink-hub` | the hub | read `roclink/in/#` and `roclink/svc/+/down`; write `svc`, `pub`, `ops`, `out` |
| `roclink-ops` | operators' tools | read `roclink/ops/#` |
| `admin` | debugging (`roclink watch`) | everything |
| anyone else | a service, named after its `[[route]]` | read `roclink/svc/<user>/#`, write `roclink/svc/<user>/down` |

No anonymous access.

## Adding a service

A service is a broker user with the same name as its `[[route]]` in the hub's config.

1. A password, in the vault:

   ```bash
   ansible-vault edit group_vars/all/vault.yml
   # add:  vault_mqtt_<name>: "<openssl rand -base64 24>"
   ```

2. The user, in `group_vars/all/main.yml` under `mqtt_users`:

   ```yaml
     <name>: "{{ vault_mqtt_<name> }}"
   ```

3. Its route, in `roles/apps/templates/hub.toml.j2`:

   ```toml
   [[route]]
   service = "<name>"
   net = 0x42
   types = ["..."]
   ```

4. Deploy both: `ansible-playbook site.yml --tags mqtt,apps`.

The service connects to `192.168.8.134:8883` over TLS, trusting `hub-ca.crt`, with that user and password. It subscribes to `roclink/svc/<name>/up`, and may publish downlink requests to `roclink/svc/<name>/down`. If it runs on the hub, as a container, it's a new unit in `roles/apps` like `scavhunt.container.j2`, with `Network=host`, and it connects to `127.0.0.1:1883`, in plain MQTT.

## Rotating a password

1. `ansible-vault edit group_vars/all/vault.yml`, and change it.
2. `ansible-playbook site.yml --tags mqtt,apps`: the broker and the hub's own services get the new one.
3. The clients that aren't on the hub:
   - **`roclink-gw`.** The gateways have it in *their* vault: change `vault_mqtt_roclink_gw` in `rfgateways/roclink-gateway/group_vars/all/vault.yml` to the same value, and run `ansible-playbook site.yml --tags gw` there. Until then the gateways can't log in, so do both together.
   - **`admin`, `roclink-ops`,** and any service elsewhere: whoever uses them needs the new one.

The broker restarts when the password file changes. Clients reconnect on their own within seconds.
