# Setting up the hub from a fresh Debian install

From Debian 13's netinstall (no desktop, "SSH server" and "standard system utilities" ticked) to a running hub. About half an hour, most of it the update and the image pulls.

You need, on your machine, in `infra/roclink-hubhost`: `ansible` with `ansible.posix`, `.vault-pass` (from a machine that has it), `~/.ssh/id_ed25519_rocadmin` (`ssh-keygen -t ed25519 -f ~/.ssh/id_ed25519_rocadmin` if this is a new admin key; then it's the key, and the old one stops working on the next run), and `pki/` if you have it, so the admin certificates stay the same.

## 1. Bootstrap the admin user

The installer made `rocuser`, with a password. Open a control connection as rocuser, which asks for that password once, and run the bootstrap through it:

```bash
cd infra/roclink-hubhost
ssh -M -S /tmp/.jvssh -fN rocuser@192.168.8.134
ansible-playbook bootstrap.yml -K          # asks for rocuser's sudo password
```

If rocuser can't sudo (the installer was given a root password), use root's password through su instead:

```bash
ansible-playbook bootstrap.yml -K -e ansible_become_method=su
```

It makes `rocadmin` with your key and passwordless sudo, and checks that both work. Nothing else changes yet: SSH still takes passwords. Close the control connection:

```bash
ssh -S /tmp/.jvssh -O exit rocuser@192.168.8.134
```

## 2. Everything else

```bash
ansible-playbook site.yml
```

In order: updates (and a reboot if the kernel changed), hardening (from here SSH takes keys only), podman, the broker, the hub and the game (built on the box), the certificates and Caddy, Beszel, backups. Each role checks its own work: the broker with a publish, the hub's API and the scoreboard over HTTP, the admin pages with and without a certificate.

It also writes `beszel-hub.pub` next to `site.yml`: Beszel's public key, which the gateway playbook gives to the gateways' agents. Commit it.

To run it in stages, use the tags in order: `--tags base`, `harden`, `podman`, `mqtt`, `apps`, `web`, `beszel`, `backup`.

## 3. By hand, once

1. Install your client certificate and trust the admin CA in your browser ([admin-certs.md](admin-certs.md)).
2. Open `https://192.168.8.134/`, make Beszel's admin account, and add this host and the gateways ([beszel.md](beszel.md)).
3. If the gateways still use the Pi's broker: [gateway-cutover.md](gateway-cutover.md).
4. Check: `ssh rocuser@192.168.8.134` with a password is refused, `http://192.168.8.134/` shows the scoreboard, and `roclink status` shows six services running.

## Reinstalling over a dead box

Same steps. Before step 3, restore the databases ([backup-restore.md](backup-restore.md)) if you have the backups. If you lost Beszel's key with the box, run the gateway playbook with `--tags beszel` after this one: it gives the gateways the new key.

The box's SSH host key is new after a reinstall. Remove the old one from your `known_hosts`: `ssh-keygen -R 192.168.8.134`.
