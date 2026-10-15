"""mosquitto_hash: a password_file line's hash, as mosquitto 2.0 writes it.

    $7$<iterations>$<base64 salt>$<base64 PBKDF2-HMAC-SHA512(password, salt)>

The salt is derived from the password and user name, so the same password
always gives the same line: the file only changes, and the broker only
restarts, when a password does.
"""

import base64
import hashlib
import hmac

ITERATIONS = 101  # mosquitto 2.0's default
SALT_LEN = 12


def mosquitto_hash(password: str, user: str) -> str:
    salt = hmac.new(password.encode(), b"mosquitto salt:" + user.encode(), hashlib.sha256).digest()[:SALT_LEN]
    digest = hashlib.pbkdf2_hmac("sha512", password.encode(), salt, ITERATIONS)
    b64 = lambda b: base64.b64encode(b).decode()
    return f"$7${ITERATIONS}${b64(salt)}${b64(digest)}"


class FilterModule:
    def filters(self):
        return {"mosquitto_hash": mosquitto_hash}
