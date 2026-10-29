# Getting back in without the key

SSH takes only keys, and only `rocadmin`'s key has sudo. If that key is lost, or SSH is broken, use the console: a keyboard and screen on the box.

## At the console

Log in as `rocadmin` with its console password:

```bash
ansible-vault view group_vars/all/vault.yml     # vault_rocadmin_password, on your machine
```

(It's only for the console: SSH refuses passwords.) Then `sudo -i` works without a password.

## A new key

On your machine:

```bash
ssh-keygen -t ed25519 -f ~/.ssh/id_ed25519_rocadmin     # replacing the lost one
cat ~/.ssh/id_ed25519_rocadmin.pub
```

At the console, put that line in `/home/rocadmin/.ssh/authorized_keys` (replacing the old one). From then on, `ansible-playbook site.yml` keeps it there: the playbook installs whatever `~/.ssh/id_ed25519_rocadmin.pub` is on the machine it runs from.

If the old key may be in someone else's hands, that's all it takes to shut it out: the playbook allows only the current key (`exclusive`).

## SSH is broken (sshd won't start)

At the console:

```bash
sudo sshd -t                                  # says what's wrong
sudo rm /etc/ssh/sshd_config.d/00-roclink.conf && sudo systemctl restart ssh
```

Then fix it in `roles/harden/templates/sshd.conf.j2` and run `--tags harden`.

## The firewall shuts you out

At the console, `sudo nft delete table inet roclink` lets everything in until the next reload or reboot. Fix `hub_lan` or `roles/harden/templates/nftables.conf.j2`, and run `--tags harden`.

## The console password doesn't work either

Boot into recovery: at GRUB, `e` on the Debian entry, add `init=/bin/bash` at the end of the `linux` line, Ctrl-X. Then:

```bash
mount -o remount,rw /
passwd rocadmin
sync; reboot -f
```

and put the new password's hash in the vault (`openssl passwd -6`) as `vault_rocadmin_password_hash`, with the password as `vault_rocadmin_password`. The playbook sets the password only when it creates the user, so it won't change it back.
