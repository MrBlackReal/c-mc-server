#!/usr/bin/env python3
"""Secure chat with enforce-secure-profile (tools/test_chat.sh runs it against both servers, online,
with the stand-in services of tools/mock_services.py whose key signs the test players' chat keys):

- a chat session (a profile key signed by the services key) is shared with everyone (INITIALIZE_CHAT)
- signed chat reaches everyone as player chat with its signature; a reply that acknowledges it
  carries it in last-seen, packed as an index into the receiver's signature cache
- a signed /say goes out as player chat with the say_command chat type
- a bad message signature is refused and breaks the sender's chain; chat without a session is refused
- a profile key the services didn't sign gets the player disconnected

    test_chat.py HOST PORT SERVICES_PORT
"""
import base64
import json
import os
import struct
import sys
import time
import urllib.request
import uuid

sys.path.insert(0, os.path.dirname(__file__))
from cryptography.hazmat.primitives import hashes, serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import padding, rsa  # noqa: E402

import test_online  # noqa: E402
from mcprobe import enc_str, enc_varint  # noqa: E402
from test_building import SB, check, failures  # noqa: E402
from test_online import OnlineClient  # noqa: E402

from test_building import SYNCED  # noqa: E402

CHAT_TYPE = dict(enumerate(SYNCED["minecraft:chat_type"]))
test_online.ACCOUNTS.update({"Carol": "22334455667788990011aabbccddeeff", "Dave": "33445566778899001122aabbccddeeff"})


def java_hash(sig):  # Arrays.hashCode(byte[])
    h = 1
    for b in sig:
        h = (31 * h + (b - 256 if b > 127 else b)) & 0xFFFFFFFF
    return h


def checksum(sigs):  # LastSeenMessages.computeChecksum
    c = 1
    for s in sigs:
        c = (31 * c + java_hash(s)) & 0xFFFFFFFF
    c &= 0xFF
    return c if c else 1


class ChatClient(OnlineClient):
    def __init__(self, host, port, name):
        self.chat_events = []
        self.seen = [None] * 20  # LastSeenMessagesTracker
        self.tail = 0
        self.offset = 0
        self.last_tracked = None
        self.chain_index = 0
        super().__init__(host, port, name)

    def _handle(self, n, r):
        if n == "player_chat":
            m = {"global": r.varint(), "sender": uuid.UUID(bytes=r.bytes(16)), "index": r.varint()}
            m["signature"] = r.bytes(256) if r.bool() else None
            m["content"], m["time"], m["salt"] = r.str(), r.i64(), r.i64()
            m["last_seen"] = []
            for _ in range(r.varint()):
                i = r.varint() - 1
                m["last_seen"].append(("full", r.bytes(256)) if i == -1 else ("cached", i))
            m["unsigned"] = r.nbt() if r.bool() else None
            m["filter"] = r.varint()
            m["chat_type"] = CHAT_TYPE[r.varint() - 1]
            m["name"] = r.nbt()
            self.chat_events.append(("player_chat", m))
            if m["signature"] and m["signature"] != self.last_tracked:  # addPending (shown)
                self.last_tracked = m["signature"]
                self.seen[self.tail] = m["signature"]
                self.tail = (self.tail + 1) % 20
                self.offset += 1
            return
        if n == "player_info_update":
            actions = r.u8()
            if actions == 0x02:  # INITIALIZE_CHAT only
                for _ in range(r.varint()):
                    who = uuid.UUID(bytes=r.bytes(16))
                    session = uuid.UUID(bytes=r.bytes(16)) if r.bool() else None
                    self.chat_events.append(("init_chat", (who, session)))
                return
            # (the full lists at login aren't looked at)
            return
        if n in ("system_chat", "disguised_chat"):
            self.chat_events.append((n, r.rest()))
            return
        super()._handle(n, r)

    def start_session(self, services_key, sign_with=None):
        self.profile_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        self.session_id = uuid.uuid4()
        der = self.profile_key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
        expires = int(time.time() * 1000) + 24 * 3600 * 1000
        payload = self.login_uuid.bytes + struct.pack(">q", expires) + der  # ProfilePublicKey.Data.signedPayload
        signature = (sign_with or services_key).sign(payload, padding.PKCS1v15(), hashes.SHA1())
        self.send("chat_session_update", self.session_id.bytes + struct.pack(">q", expires) + enc_varint(len(der)) + der
                  + enc_varint(len(signature)) + signature)

    def last_seen_update(self):  # generateAndApplyUpdate
        offset, self.offset = self.offset, 0
        acknowledged, seen = 0, []
        for i in range(20):
            e = self.seen[(self.tail + i) % 20]
            if e is not None:
                acknowledged |= 1 << i
                seen.append(e)
        return enc_varint(offset) + acknowledged.to_bytes(3, "little") + bytes([checksum(seen)]), seen

    def sign(self, content, stamp, salt, seen):
        payload = struct.pack(">i", 1) + self.login_uuid.bytes + self.session_id.bytes + struct.pack(">i", self.chain_index)
        body = content.encode()
        payload += struct.pack(">qq", salt, stamp // 1000) + struct.pack(">i", len(body)) + body + struct.pack(">i", len(seen)) + b"".join(seen)
        self.chain_index += 1
        return self.profile_key.sign(payload, padding.PKCS1v15(), hashes.SHA256())

    def chat(self, content, signed=True, forge=False):
        stamp, salt = int(time.time() * 1000), int.from_bytes(os.urandom(8), "big", signed=True)
        update, seen = self.last_seen_update()
        signature = self.sign(content, stamp, salt, seen) if signed else None
        if forge:
            signature = os.urandom(256)
        self.send("chat", enc_str(content) + struct.pack(">qq", stamp, salt) + (b"\x01" + signature if signature else b"\x00") + update)
        return signature

    def signed_say(self, text):
        stamp, salt = int(time.time() * 1000), int.from_bytes(os.urandom(8), "big", signed=True)
        update, seen = self.last_seen_update()
        signature = self.sign(text, stamp, salt, seen)
        self.send("chat_command_signed", enc_str("say " + text) + struct.pack(">qq", stamp, salt) + enc_varint(1) + enc_str("message")
                  + signature + update)

    def chats(self, kind="player_chat"):
        return [m for n, m in self.chat_events if n == kind]


def main():
    host, port, services = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    test_online.SERVICES = services
    key_der = base64.b64decode(json.load(urllib.request.urlopen(f"http://127.0.0.1:{services}/test/services-key"))["privateKey"])
    services_key = serialization.load_der_private_key(key_der, None)

    alice = ChatClient(host, port, "Alice")
    bob = ChatClient(host, port, "Bob")
    alice.pump(1.0)
    bob.pump(1.0)
    alice.start_session(services_key)
    bob.start_session(services_key)
    got = bob.pump(until=lambda: any(e[0] == alice.login_uuid for n, e in bob.chat_events if n == "init_chat"), timeout=5.0)
    shared = [e for n, e in bob.chat_events if n == "init_chat" and e[0] == alice.login_uuid]
    check(got and shared[0][1] == alice.session_id, f"Alice's chat session reached Bob: {shared}")
    alice.pump(until=lambda: any(e[0] == bob.login_uuid for n, e in alice.chat_events if n == "init_chat"), timeout=5.0)

    sig = alice.chat("hello bob")
    bob.pump(until=lambda: bob.chats(), timeout=5.0)
    alice.pump(until=lambda: alice.chats(), timeout=5.0)
    got = bob.chats()
    print(f"OBS bob got: {[(m['content'], m['index'], m['chat_type'], m['filter'], m['last_seen']) for m in got]}")
    check(got and got[0]["sender"] == alice.login_uuid and got[0]["signature"] == sig and got[0]["content"] == "hello bob",
          "Bob got Alice's signed message")
    check(got and got[0]["index"] == 0 and got[0]["chat_type"] == "minecraft:chat" and got[0]["unsigned"] is None and got[0]["filter"] == 0,
          "index 0, chat type minecraft:chat, no unsigned content, not filtered")
    check(got and b"Alice" in json.dumps(got[0]["name"]).encode(), f"the sender's name: {got[0]['name'] if got else None}")

    reply = bob.chat("hi alice")  # acknowledges Alice's message
    alice.pump(until=lambda: len(alice.chats()) >= 2, timeout=5.0)
    got = alice.chats()
    print(f"OBS alice got: {[(m['content'], m['index'], m['last_seen']) for m in got]}")
    check(len(got) >= 2 and got[1]["content"] == "hi alice" and got[1]["signature"] == reply, "Alice got Bob's reply")
    check(len(got) >= 2 and got[1]["last_seen"] == [("cached", 0)], f"its last-seen refers to Alice's cached message: {got[1]['last_seen'] if len(got) > 1 else None}")

    alice.signed_say("announcement")
    bob.pump(until=lambda: len(bob.chats()) >= 3, timeout=5.0)
    got = bob.chats()
    print(f"OBS say: {[(m['content'], m['index'], m['chat_type']) for m in got[2:]]}")
    check(len(got) >= 3 and got[2]["chat_type"] == "minecraft:say_command" and got[2]["content"] == "announcement" and got[2]["index"] == 1,
          "a signed /say is player chat with the say_command type (index 1)")

    alice.pump(until=lambda: len(alice.chats()) >= 3, timeout=5.0)  # (the /say)
    before = len(alice.chats())
    disabled = lambda c: [m for m in c.chats("system_chat") if b"chat.disabled" in m]
    bob.chat("forged", forge=True)
    bob.pump(until=lambda: disabled(bob), timeout=5.0)
    refusals = [m for m in bob.chats("system_chat") if b"chat.disabled" in m]
    print(f"OBS forged: {refusals}")
    check(refusals and b"chat.disabled.invalid_signature" in refusals[0], "a forged signature is refused")
    bob.chat("after the break")
    bob.pump(until=lambda: len([m for m in bob.chats("system_chat") if b"chat.disabled" in m]) >= 2, timeout=5.0)
    refusals = [m for m in bob.chats("system_chat") if b"chat.disabled" in m]
    check(len(refusals) >= 2 and b"chat.disabled.chain_broken" in refusals[1], f"then the chain is broken: {refusals[1:]}")
    alice.pump(1.0)
    check(len(alice.chats()) == before, "nobody got those")

    carol = ChatClient(host, port, "Carol")
    carol.pump(0.5)
    carol.chat("no session", signed=False)
    carol.pump(until=lambda: disabled(carol), timeout=5.0)
    refusals = [m for m in carol.chats("system_chat") if b"chat.disabled" in m]
    check(refusals and b"chat.disabled.missingProfileKey" in refusals[0], f"chat without a session is refused: {refusals}")

    dave = ChatClient(host, port, "Dave")
    dave.pump(0.5)
    dave.start_session(services_key, sign_with=rsa.generate_private_key(public_exponent=65537, key_size=2048))
    dave.pump(until=lambda: dave.saw("disconnect"), timeout=5.0)
    reason = [i for n, i in dave.events if n == "disconnect"]
    check(reason and b"multiplayer.disconnect.invalid_public_key_signature" in reason[0], f"an unsigned chat key disconnects: {reason}")
    print()
    if failures:
        print(f"{len(failures)} check(s) failed")
        sys.exit(1)
    print("all chat checks passed")


if __name__ == "__main__":
    main()
