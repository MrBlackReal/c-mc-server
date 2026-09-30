#!/usr/bin/env python3
"""Headless Minecraft protocol probe.

Logs into a server in offline mode, walks through the configuration phase,
enters play and records what the server sends. Used both to capture reference
data from the vanilla server and as a smoke test for the C++ server.

    mcprobe.py status  HOST PORT
    mcprobe.py join    HOST PORT [--name N] [--seconds S] [--dump FILE] [--packets packets.json]
"""
import argparse
import json
import os
import socket
import struct
import sys
import time
import uuid
import zlib

PROTOCOL = 777


# ---------------------------------------------------------------- primitives
def enc_varint(v):
    v &= 0xFFFFFFFF
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def enc_str(s):
    b = s.encode()
    return enc_varint(len(b)) + b


class Reader:
    def __init__(self, data):
        self.d = data
        self.i = 0

    def remaining(self):
        return len(self.d) - self.i

    def bytes(self, n):
        if self.i + n > len(self.d):
            raise EOFError(f"need {n} bytes at {self.i}, have {len(self.d) - self.i}")
        b = self.d[self.i:self.i + n]
        self.i += n
        return b

    def u8(self):
        return self.bytes(1)[0]

    def bool(self):
        return self.u8() != 0

    def i8(self):
        return struct.unpack(">b", self.bytes(1))[0]

    def i16(self):
        return struct.unpack(">h", self.bytes(2))[0]

    def u16(self):
        return struct.unpack(">H", self.bytes(2))[0]

    def i32(self):
        return struct.unpack(">i", self.bytes(4))[0]

    def i64(self):
        return struct.unpack(">q", self.bytes(8))[0]

    def f32(self):
        return struct.unpack(">f", self.bytes(4))[0]

    def f64(self):
        return struct.unpack(">d", self.bytes(8))[0]

    def varint(self):
        n = 0
        for shift in range(0, 35, 7):
            b = self.u8()
            n |= (b & 0x7F) << shift
            if not b & 0x80:
                return n - (1 << 32) if n & 0x80000000 else n
        raise ValueError("varint too long")

    def str(self):
        return self.bytes(self.varint()).decode()

    def uuid(self):
        return str(uuid.UUID(bytes=self.bytes(16)))

    def rest(self):
        return self.bytes(self.remaining())

    # Network NBT: root has a type byte but no name.
    def nbt(self):
        t = self.u8()
        if t == 0:
            return None
        return self._nbt_payload(t)

    def _nbt_str(self):
        return self.bytes(self.u16()).decode("utf-8", "replace")

    def _nbt_payload(self, t):
        if t == 1:
            return self.i8()
        if t == 2:
            return self.i16()
        if t == 3:
            return self.i32()
        if t == 4:
            return self.i64()
        if t == 5:
            return self.f32()
        if t == 6:
            return self.f64()
        if t == 7:
            return list(self.bytes(self.i32()))
        if t == 8:
            return self._nbt_str()
        if t == 9:
            et = self.u8()
            n = self.i32()
            return [self._nbt_payload(et) for _ in range(n)]
        if t == 10:
            out = {}
            while True:
                ct = self.u8()
                if ct == 0:
                    return out
                name = self._nbt_str()
                out[name] = self._nbt_payload(ct)
        if t == 11:
            return [self.i32() for _ in range(self.i32())]
        if t == 12:
            return [self.i64() for _ in range(self.i32())]
        raise ValueError(f"bad nbt tag {t}")


# ---------------------------------------------------------------- connection
class Conn:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=10)
        self.buf = b""
        self.threshold = -1
        self.encryptor = self.decryptor = None

    def enable_encryption(self, secret):
        """AES/CFB8 with the shared secret as key and IV, both ways (online-mode login)."""
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms
        try:
            from cryptography.hazmat.decrepit.ciphers.modes import CFB8
        except ImportError:
            from cryptography.hazmat.primitives.ciphers.modes import CFB8
        cipher = Cipher(algorithms.AES(secret), CFB8(secret))
        self.encryptor, self.decryptor = cipher.encryptor(), cipher.decryptor()
        self.buf = self.decryptor.update(self.buf)

    def send(self, pid, payload=b""):
        body = enc_varint(pid) + payload
        if self.threshold >= 0:
            if len(body) >= self.threshold:
                body = enc_varint(len(body)) + zlib.compress(body)
            else:
                body = enc_varint(0) + body
        frame = enc_varint(len(body)) + body
        self.s.sendall(self.encryptor.update(frame) if self.encryptor else frame)

    def _fill(self):
        chunk = self.s.recv(65536)
        if not chunk:
            raise ConnectionError("server closed connection")
        self.buf += self.decryptor.update(chunk) if self.decryptor else chunk

    def recv(self):
        while True:
            r = Reader(self.buf)
            try:
                length = r.varint()
                if r.remaining() >= length:
                    body = r.bytes(length)
                    self.buf = self.buf[r.i:]
                    break
            except EOFError:
                pass
            self._fill()
        r = Reader(body)
        if self.threshold >= 0:
            data_len = r.varint()
            body = zlib.decompress(r.rest()) if data_len else r.rest()
            r = Reader(body)
        pid = r.varint()
        return pid, r


def load_names(path):
    names = {}
    if path and os.path.exists(path):
        for state, dirs in json.load(open(path)).items():
            for d, pk in dirs.items():
                for k, v in pk.items():
                    names[(state, d, v["protocol_id"])] = k.split(":", 1)[1]
    return names


def handshake(c, host, port, intent):
    c.send(0, enc_varint(PROTOCOL) + enc_str(host) + struct.pack(">H", port) + enc_varint(intent))


def cmd_status(a):
    c = Conn(a.host, a.port)
    handshake(c, a.host, a.port, 1)
    c.send(0)
    pid, r = c.recv()
    print(json.dumps(json.loads(r.str()), indent=2))
    t = int(time.time() * 1000)
    c.send(1, struct.pack(">q", t))
    pid, r = c.recv()
    assert pid == 1 and r.i64() == t, "bad pong"
    print("pong ok")


def cmd_join(a):
    names = load_names(a.packets)
    nm = lambda st, pid: names.get((st, "clientbound", pid), f"0x{pid:02x}")
    dump = {"registries": [], "tags": {}, "config_packets": [], "play_packets": []}
    c = Conn(a.host, a.port)
    handshake(c, a.host, a.port, 2)
    c.send(0, enc_str(a.name) + uuid.uuid3(uuid.NAMESPACE_DNS, a.name).bytes)

    # --- login
    while True:
        pid, r = c.recv()
        if pid == 3:
            c.threshold = r.varint()
            print(f"[login] compression threshold {c.threshold}")
        elif pid == 2:
            prof_id, prof_name = r.uuid(), r.str()
            nprops = r.varint()
            print(f"[login] finished: {prof_name} {prof_id} props={nprops}")
            c.send(3)  # login_acknowledged
            break
        elif pid == 0:
            print("[login] disconnect:", r.rest()[:500])
            return 1
        else:
            print(f"[login] unexpected packet {pid}")
            return 1

    # --- configuration
    c.send(0, enc_str("en_us") + bytes([8]) + enc_varint(0) + b"\x01" + bytes([0x7F])
           + enc_varint(1) + b"\x00" + b"\x01" + enc_varint(0))
    c.send(2, enc_str("minecraft:brand") + enc_str("mcprobe"))
    while True:
        pid, r = c.recv()
        name = nm("configuration", pid)
        dump["config_packets"].append(name)
        if name == "select_known_packs":
            packs = [(r.str(), r.str(), r.str()) for _ in range(r.varint())]
            print(f"[config] known packs {packs}")
            c.send(7, enc_varint(len(packs)) + b"".join(enc_str(x) for p in packs for x in p))
        elif name == "registry_data":
            reg = r.str()
            entries = []
            for _ in range(r.varint()):
                eid = r.str()
                data = r.nbt() if r.bool() else None
                entries.append({"id": eid, "data": data})
            with_data = sum(1 for e in entries if e["data"] is not None)
            print(f"[config] registry {reg}: {len(entries)} entries ({with_data} with data)")
            dump["registries"].append({"registry": reg, "entries": entries})
        elif name == "update_tags":
            for _ in range(r.varint()):
                reg = r.str()
                tags = {}
                for _ in range(r.varint()):
                    t = r.str()
                    tags[t] = [r.varint() for _ in range(r.varint())]
                dump["tags"][reg] = tags
            print(f"[config] tags for {len(dump['tags'])} registries")
        elif name == "update_enabled_features":
            feats = [r.str() for _ in range(r.varint())]
            dump["features"] = feats
            print(f"[config] features {feats}")
        elif name == "custom_payload":
            ch = r.str()
            print(f"[config] custom_payload {ch} {r.rest()[:64]!r}")
        elif name == "keep_alive":
            c.send(4, r.bytes(8))
        elif name == "ping":
            c.send(5, r.bytes(4))
        elif name == "code_of_conduct":
            print(f"[config] code of conduct: {r.str()[:80]!r}")
            c.send(9)
        elif name == "disconnect":
            print("[config] disconnect:", r.nbt())
            return 1
        elif name == "finish_configuration":
            print("[config] finish")
            c.send(3)
            break
        else:
            print(f"[config] {name} ({r.remaining()} bytes)")

    # --- play
    deadline = time.time() + a.seconds
    counts = {}
    got_position = False
    next_step = 0
    pos = (0, 0, 0)
    c.s.settimeout(0.05)
    while time.time() < deadline:
        if a.walk and got_position and time.time() > next_step:
            next_step = time.time() + 0.05
            pos = (pos[0] + 0.4, pos[1], pos[2])  # ~8 blocks/s
            c.send(13)  # client_tick_end: a real client ends a tick before each move (one position per tick)
            c.send(30, struct.pack(">dddB", *pos, 1))  # move_player_pos, on ground
        try:
            pid, r = c.recv()
        except socket.timeout:
            continue
        except ConnectionError as e:
            print(f"[play] {e}")
            break
        name = nm("play", pid)
        counts[name] = counts.get(name, 0) + 1
        if counts[name] <= 2 and name != "level_chunk_with_light":
            dump["play_packets"].append({"name": name, "hex": r.d[r.i:].hex()[:4000]})
        if name == "level_chunk_with_light" and r.d[r.i:r.i + 8] == bytes(8):
            dump["chunk_0_0"] = r.d[r.i:].hex()
        if name == "keep_alive":
            c.send(28, r.bytes(8))
        elif name == "ping":
            c.send(45, r.bytes(4))
        elif name == "login":
            eid = r.i32()
            hc = r.bool()
            levels = [r.str() for _ in range(r.varint())]
            print(f"[play] login eid={eid} hardcore={hc} levels={levels} "
                  f"max={r.varint()} view={r.varint()} sim={r.varint()}")
        elif name == "player_position":
            tid = r.varint()
            x, y, z = r.f64(), r.f64(), r.f64()
            r.f64(), r.f64(), r.f64()  # delta movement
            yrot, xrot = r.f32(), r.f32()
            print(f"[play] position tp={tid} ({x:.2f}, {y:.2f}, {z:.2f})")
            c.send(0, enc_varint(tid) + struct.pack(">dddff", x, y, z, yrot, xrot))  # accept_teleportation
            if not got_position:
                got_position = True
                c.send(44)  # player_loaded
                if a.chat:
                    c.send(9, enc_str(a.chat) + struct.pack(">qq", int(time.time() * 1000), 0)
                           + b"\x00" + enc_varint(0) + bytes(3) + b"\x00")
                pos = (x, y, z)
        elif name == "system_chat":
            print("[play] chat:", json.dumps(r.nbt()))
        elif name == "player_chat":  # index, sender, index, signature?, then the signed body's text
            r.varint()
            sender = r.uuid()
            r.varint()
            if r.bool():
                r.bytes(256)
            print(f"[play] player chat from {sender}: {json.dumps(r.str())}")
        elif name == "disguised_chat":
            print("[play] disguised chat:", json.dumps(r.nbt()))
        elif name == "add_entity":
            eid = r.varint()
            print(f"[play] add_entity id={eid} uuid={r.uuid()} type={r.varint()} at ({r.f64():.2f}, {r.f64():.2f}, {r.f64():.2f})")
        elif name == "chunk_batch_finished":
            c.send(11, struct.pack(">f", 25.0))
        elif name == "disconnect":
            print("[play] disconnect:", r.nbt())
            break
    print("[play] packet counts:", json.dumps(counts, sort_keys=True))
    if a.dump:
        with open(a.dump, "w") as f:
            json.dump(dump, f, indent=1)
        print(f"wrote {a.dump}")
    ok = got_position and counts.get("level_chunk_with_light", 0) > 0
    print("RESULT:", "OK" if ok else "FAIL")
    return 0 if ok else 1


def main():
    global PROTOCOL
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("status")
    s.add_argument("host")
    s.add_argument("port", type=int)
    j = sub.add_parser("join")
    j.add_argument("host")
    j.add_argument("port", type=int)
    j.add_argument("--name", default="Probe")
    j.add_argument("--seconds", type=float, default=5)
    j.add_argument("--dump")
    j.add_argument("--chat", help="send this chat message after spawning")
    j.add_argument("--walk", action="store_true", help="walk along +x after spawning")
    j.add_argument("--packets", default=os.path.join(os.path.dirname(__file__), "..", "data", "packets.json"))
    for sp in (s, j):
        sp.add_argument("--protocol", type=int, default=PROTOCOL)
    a = p.parse_args()
    PROTOCOL = a.protocol
    sys.exit({"status": cmd_status, "join": cmd_join}[a.cmd](a) or 0)


if __name__ == "__main__":
    main()
