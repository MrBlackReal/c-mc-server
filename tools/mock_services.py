#!/usr/bin/env python3
"""A stand-in for the Minecraft services an online-mode server talks to, for tests:

    GET  /minecraft/client            the discovery document (session/verify points back here)
    GET  /session/verify?username=&serverId=[&ip=]
                                      a profile if that player "joined" with that server hash, else 204
    GET  /publickeys                  the services key set (a key made at startup signs chat keys)
    POST /test/join?username=&serverId=&id=   what a client's join request would record
    GET  /test/services-key           that key's private half (PKCS#8, base64), to sign test chat keys

Point a server at it with MINECRAFT_API_DISCOVERY_HOST=http://127.0.0.1:PORT/minecraft/client
(vanilla: -Dminecraft.api.discovery.host=...).

    mock_services.py PORT
"""
import base64
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from cryptography.hazmat.primitives import serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import rsa  # noqa: E402

joined = {}  # username -> (server hash, undashed uuid)
lock = threading.Lock()
SERVICES_KEY = rsa.generate_private_key(public_exponent=65537, key_size=2048)
SERVICES_PUBLIC = base64.b64encode(SERVICES_KEY.public_key().public_bytes(
    serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)).decode()
SERVICES_PRIVATE = base64.b64encode(SERVICES_KEY.private_bytes(
    serialization.Encoding.DER, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())).decode()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, code, body=None):
        data = json.dumps(body).encode() if body is not None else b""
        self.send_response(code)
        if body is not None:
            self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        port = self.server.server_address[1]
        if url.path == "/minecraft/client":
            endpoint = {"uri": f"http://127.0.0.1:{port}/session/verify", "validUris": []}
            self.reply(200, {"environment": "test", "product": "minecraft",
                             "discovery": {"product": "minecraft",
                                           "authentication": {"endpoints": {"getPublicKeys": {
                                               "uri": f"http://127.0.0.1:{port}/publickeys", "validUris": []}}},
                                           "session": {"endpoints": {"verify": endpoint}},
                                           "player": {"endpoints": {}}, "profiles": {"endpoints": {}}, "telemetry": {"endpoints": {}}}})
        elif url.path == "/session/verify":
            with lock:
                entry = joined.get(q.get("username"))
            if not entry or entry[0] != q.get("serverId"):
                self.reply(204)
                return
            self.reply(200, {"id": entry[1], "name": q["username"],
                             "properties": [{"name": "textures", "value": "dGV4dHVyZXM=", "signature": "c2lnbmF0dXJl"}]})
        elif url.path == "/publickeys":
            self.reply(200, {"profilePropertyKeys": [], "playerCertificateKeys": [{"publicKey": SERVICES_PUBLIC}]})
        elif url.path == "/test/services-key":
            self.reply(200, {"privateKey": SERVICES_PRIVATE})
        else:
            self.reply(404, {"error": "not found"})

    def do_POST(self):
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        if url.path == "/test/join":
            with lock:
                joined[q["username"]] = (q["serverId"], q["id"])
            self.reply(204)
        else:
            self.reply(404, {"error": "not found"})


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
