# Admin client certificates

The admin pages (`https://192.168.8.134/` for Beszel, `:8443` for the hub's API) let in only a browser that presents a client certificate of someone in `admin_clients` (`group_vars/all/main.yml`). Without one the TLS handshake fails: the browser shows a connection error, not a page.

Everything is made on your machine by `roles/pki`, in `pki/` (git-ignored, back it up with `.vault-pass`):

| File | What | Lasts |
|---|---|---|
| `pki/ca.key`, `ca.crt` | the admin CA: it signs the others, and only for `192.168.8.0/24` and `roclink-hub` (its name constraints). `ca.crt` is also `hub-ca.crt`, committed, for the gateways | 10 years |
| `pki/server.key`, `server.crt` | the hub's certificate, for `192.168.8.134` and `roclink-hub`: Caddy's, and Mosquitto's for MQTT over TLS | 825 days |
| `pki/clients/<name>.p12` | a person's certificate and key, for a browser | 2 years |

The `.p12` files' passphrase is `vault_p12_password`: `ansible-vault view group_vars/all/vault.yml`.

## Installing yours

1. **Trust the admin CA** (so the browser trusts the hub's HTTPS certificate). Import `pki/ca.crt` as a certificate authority, trusted for websites:
   - Firefox: Settings → Privacy & Security → Certificates → View Certificates → Authorities → Import.
   - Chrome, Edge (Linux): Settings → Privacy and security → Security → Manage certificates → Authorities → Import.
   - macOS (Safari, Chrome): open `ca.crt` in Keychain Access, then double-click it → Trust → "Always Trust".
   - Windows: `certmgr.msc` → Trusted Root Certification Authorities → Import.
2. **Install your certificate**, `pki/clients/<name>.p12`, with the passphrase:
   - Firefox: the same dialog → Your Certificates → Import.
   - Chrome, Edge (Linux): Manage certificates → Your certificates → Import.
   - macOS: open the `.p12` (it goes into the login keychain).
   - Windows: double-click the `.p12`, Current User.
3. Open `https://192.168.8.134/`. The browser asks which certificate to present; choose "roclink hub admin (<name>)".

From a shell:

```bash
curl --cacert pki/ca.crt --cert pki/clients/<name>.crt --key pki/clients/<name>.key \
  https://192.168.8.134:8443/api/nodes
```

## Issuing one for someone else

Add their name to `admin_clients` and run:

```bash
ansible-playbook site.yml --tags web
```

Give them `pki/clients/<name>.p12`, `pki/ca.crt`, and the passphrase, over separate channels.

## Revoking one

Remove the name from `admin_clients` and run `--tags web`. Caddy accepts only the certificates of the names in the list, so it's shut out at once, although the CA still signed it. Delete its files from `pki/clients/` so it isn't issued again by accident.

## Replacing the CA

Do this if the CA's key (`pki/ca.key`) may have leaked. Also do it if the playbook stops at "The CA is limited to the hub's names", which means the CA predates its name constraints.

1. Move `pki/` aside. Don't delete it until the new one works.
2. Run `ansible-playbook site.yml --tags mqtt,web`. That makes a new CA, server certificate and client certificates, writes the new `hub-ca.crt`, and installs them on the hub.
3. Commit `hub-ca.crt`, and run the gateway playbook's `--tags gw`. Until you do, the gateways refuse the broker's new certificate.
4. In each admin's browser, remove the old CA and certificate, and install the new ones.

If the hub's network changes (`hub_lan`), the CA must be replaced too, because its name constraints name the network.

## Renewing

The client certificates last 2 years, the server's 825 days. A browser that gets "certificate expired" needs a new one: delete the old files (`pki/clients/<name>.*`, or `pki/server.*`) and run `--tags web` (`--tags mqtt,web` for the server's, which the broker uses too). A new server certificate needs nothing on the browsers' or the gateways' side; a new client certificate has to be installed again.

```bash
openssl x509 -enddate -noout -in pki/server.crt
openssl x509 -enddate -noout -in pki/clients/<name>.crt
```

If the hub's address changes, delete `pki/server.*` too: the certificate names the address.

## If macOS won't import the .p12

Old macOS versions can't read OpenSSL 3's encryption. Make a copy in the old format:

```bash
openssl pkcs12 -in pki/clients/<name>.p12 -nodes | openssl pkcs12 -export -legacy -out <name>-legacy.p12
```
