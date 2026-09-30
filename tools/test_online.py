#!/usr/bin/env python3
"""Online-mode logins (tools/test_online.sh runs this against both servers, with the stand-in
services of tools/mock_services.py):

- the server asks for encryption; the client answers with a shared secret, the login continues
  encrypted, and the session server's profile (its id and skin property) is what the player gets
- a player who didn't join through the session server is turned away (unverified_username)
- logging in again with the same account kicks the first connection (duplicate_login)

    test_online.py HOST PORT SERVICES_PORT
"""
import hashlib
import os
import sys
import urllib.request
import uuid

sys.path.insert(0, os.path.dirname(__file__))
from cryptography.hazmat.primitives.asymmetric import padding  # noqa: E402
from cryptography.hazmat.primitives.serialization import load_der_public_key  # noqa: E402

import test_building as tb  # noqa: E402
from mcprobe import enc_varint  # noqa: E402
from test_building import Client, check, failures  # noqa: E402

SERVICES = None
ACCOUNTS = {"Alice": "0f4c2a7e1b3d4c5e8f9a0b1c2d3e4f50", "Bob": "11223344556677889900aabbccddeeff"}


def server_hash(server_id, secret, public_key):
    """Crypt.digestData as new BigInteger(digest).toString(16) (what the client sends to the session server)."""
    digest = hashlib.sha1(server_id.encode("latin-1") + secret + public_key).digest()
    n = int.from_bytes(digest, "big", signed=True)
    return format(n, "x") if n >= 0 else "-" + format(-n, "x")


class OnlineClient(Client):
    join_session_server = True  # a real client joins before answering; an impostor can't

    def on_encryption_request(self, r):
        server_id = r.str()
        public_key = r.bytes(r.varint())
        challenge = r.bytes(r.varint())
        self.should_authenticate = r.bool()
        self.challenge_length = len(challenge)
        secret = os.urandom(16)
        key = load_der_public_key(public_key)
        if self.join_session_server:
            h = server_hash(server_id, secret, public_key)
            urllib.request.urlopen(urllib.request.Request(
                f"http://127.0.0.1:{SERVICES}/test/join?username={self.name}&serverId={h}&id={ACCOUNTS[self.name]}", method="POST"))
        enc_secret = key.encrypt(secret, padding.PKCS1v15())
        enc_challenge = key.encrypt(challenge, padding.PKCS1v15())
        self.c.send(1, enc_varint(len(enc_secret)) + enc_secret + enc_varint(len(enc_challenge)) + enc_challenge)
        self.c.enable_encryption(secret)

    def _handle(self, n, r):
        if os.environ.get("DEBUG_PACKETS"):
            print("   packet", n)
        if n in ("disconnect", "system_chat", "player_chat", "disguised_chat"):  # (raw: their text is in there)
            self.events.append((n, r.rest()))
            return
        super()._handle(n, r)

    def pump(self, seconds=0.0, until=None, timeout=3.0):
        try:
            return super().pump(seconds, until, timeout)
        except (ConnectionError, OSError):
            return until() if until else False


def login_error(make):
    try:
        make()
    except (RuntimeError, ConnectionError) as e:
        return str(e)
    return None


def main():
    global SERVICES
    SERVICES = int(sys.argv[3])
    host, port = sys.argv[1], int(sys.argv[2])

    alice = OnlineClient(host, port, "Alice")
    alice.pump(1.0)
    print(f"OBS alice {alice.login_uuid} {alice.login_name} {alice.login_properties}")
    check(alice.should_authenticate and alice.challenge_length == 4, "encryption request: authenticate, 4 byte challenge")
    check(alice.login_uuid == uuid.UUID(ACCOUNTS["Alice"]), f"the session server's id: {alice.login_uuid}")
    check(alice.login_properties == [("textures", "dGV4dHVyZXM=", "c2lnbmF0dXJl")], f"the skin property: {alice.login_properties}")
    check(alice.pos is not None, "encrypted play: got a position")
    alice.command("say hello")
    # (/say comes back as disguised chat from vanilla: Alice has no chat session)
    heard = lambda: any(n in ("system_chat", "player_chat", "disguised_chat") and b"hello" in i for n, i in alice.events)
    check(alice.pump(until=heard, timeout=3.0), "encrypted chat round trip")

    class Impostor(OnlineClient):
        join_session_server = False
    error = login_error(lambda: Impostor(host, port, "Bob"))
    print(f"OBS impostor: {error}")
    check(error is not None and "multiplayer.disconnect.unverified_username" in error, f"unverified player refused: {error}")

    again = OnlineClient(host, port, "Alice")
    kicked = alice.pump(until=lambda: alice.saw("disconnect"), timeout=10.0)
    reason = [i for n, i in alice.events if n == "disconnect"]
    print(f"OBS duplicate: {reason}")
    check(kicked and "multiplayer.disconnect.duplicate_login" in str(reason), f"first Alice kicked: {reason}")
    again.pump(1.0)
    check(again.pos is not None, "second Alice plays")
    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        sys.exit(1)
    print("all online-mode checks passed")


if __name__ == "__main__":
    main()
