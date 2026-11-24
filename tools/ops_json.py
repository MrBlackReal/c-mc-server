#!/usr/bin/env python3
"""Prints an ops.json making the named offline-mode players operators (level 4): tools/ops_json.py Alice Bob > ops.json"""
import hashlib
import json
import sys
import uuid


def offline(name):
    h = bytearray(hashlib.md5(b"OfflinePlayer:" + name.encode()).digest())
    h[6] = h[6] & 15 | 48
    h[8] = h[8] & 63 | 128
    return str(uuid.UUID(bytes=bytes(h)))


print(json.dumps([{"uuid": offline(n), "name": n, "level": 4, "bypassesPlayerLimit": False} for n in (sys.argv[1:] or ["Alice", "Bob"])]))
