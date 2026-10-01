#!/usr/bin/env python3
"""A catalog and download server for the device, with every failure it must
survive, over HTTP and HTTPS.

    python3 tools/dev/testserver.py [--root DIR] [--http-port 80]
                                    [--https-port 443]

Run it on a machine in the same LAN as the SidecarTridge and point the app's
catalog at it (the HTTP_CATALOG setting): http://<this machine>/roms.csv. The
firmware fetches a catalog entry from the catalog's own host, at the root, on
port 80, so the default ports are 80 and 443 (macOS lets any user bind them).

Files under --root (default tools/dev/builds/testserver) are served by name,
roms.csv included: make them with make_rom_images.py and make_catalog.py.

Generated routes, for any name:
  synthetic-NNNN.img   the 64 KB pattern image (make_rom_images.py)
  fail-404*            404 with an HTML body
  fail-500*            500
  fail-html200*        200 with an HTML error page (a proxy's or captive
                       portal's), which must not become a catalog or a ROM
  fail-truncated*      a Content-Length the body never reaches, then close
  fail-stall*          headers, a little of the body, then nothing
  fail-loop*           a redirect to itself
  302, 301, rel, cd, 302cd, tohttps   the redirect cases of md-browser's
                       server, ending at file.bin (64 KB, printed MD5)

HTTPS uses a self-signed certificate made with openssl on first run, in
tools/dev/builds/testserver-cert/: the firmware encrypts but does not verify.
Every request is logged to stdout and to tools/dev/logs/testserver.log, so a
test can check what the device asked for.
"""

import argparse
import datetime
import hashlib
import os
import ssl
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from make_rom_images import pattern  # noqa: E402

PAYLOAD = bytes(range(256)) * 256          # file.bin: 64 KB, deterministic
TARGET = b"relative-redirect-target-payload\n" * 64
SYNTHETIC = pattern(64 * 1024)
CERT_DIR = os.path.join(HERE, "builds", "testserver-cert")
LOG = os.path.join(HERE, "logs", "testserver.log")
PORTS = {"http": 80, "https": 443}
ROOT = os.path.join(HERE, "builds", "testserver")
LOG_LOCK = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def scheme(self):
        return "https" if isinstance(self.connection, ssl.SSLSocket) else "http"

    def send(self, code, body=b"", ctype="application/octet-stream",
             length=None, extra=()):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length",
                         str(len(body) if length is None else length))
        for name, value in extra:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def redirect(self, code, location):
        self.send(code, extra=[("Location", location)])

    def do_GET(self):
        # lwIP's client sends Host without the port: absolute URLs are
        # rebuilt from the ports this server listens on.
        host = self.headers.get("Host", "localhost").split(":")[0]
        name = urllib.parse.unquote(self.path.split("?")[0].lstrip("/"))
        html = b"<html><body><h1>Something went wrong</h1></body></html>"
        if name.startswith("fail-404"):
            self.send(404, html, "text/html")
        elif name.startswith("fail-500"):
            self.send(500, html, "text/html")
        elif name.startswith("fail-html200"):
            self.send(200, html, "text/html")
        elif name.startswith("fail-truncated"):
            self.send(200, SYNTHETIC[:10000], length=len(SYNTHETIC))
            self.close_connection = True
        elif name.startswith("fail-stall"):
            self.send_response(200)
            self.send_header("Content-Length", str(len(SYNTHETIC)))
            self.end_headers()
            self.wfile.write(SYNTHETIC[:1000])
            self.wfile.flush()
            time.sleep(600)
        elif name.startswith("fail-loop"):
            self.redirect(302, "/" + name)
        elif name.startswith("synthetic-") and name.endswith(".img"):
            self.send(200, SYNTHETIC)
        elif name == "file.bin":
            self.send(200, PAYLOAD)
        elif name == "302":
            self.redirect(302, "/file.bin")
        elif name == "301":
            self.redirect(301, f"{self.scheme()}://{host}:"
                          f"{PORTS[self.scheme()]}/file.bin")
        elif name == "rel":
            self.redirect(302, "sub/target.bin")
        elif name == "sub/target.bin":
            self.send(200, TARGET)
        elif name == "cd":
            self.send(200, TARGET, extra=[("Content-Disposition",
                                           'attachment; filename="REALNAME.ST"')])
        elif name == "302cd":
            self.redirect(302, "/cd")
        elif name == "tohttps":
            self.redirect(302, f"https://{host}:{PORTS['https']}/file.bin")
        else:
            path = os.path.realpath(os.path.join(ROOT, name))
            if (os.path.dirname(path) == os.path.realpath(ROOT)
                    and os.path.isfile(path)):
                with open(path, "rb") as f:
                    body = f.read()
                ctype = "text/csv" if name.endswith(".csv") else \
                    "application/octet-stream"
                self.send(200, body, ctype)
            else:
                self.send(404, html, "text/html")

    def log_message(self, fmt, *args):
        line = (f"{datetime.datetime.now():%Y-%m-%d %H:%M:%S.%f}"[:23] +
                f" [{self.scheme()}] {self.address_string()} {fmt % args}")
        print(line, flush=True)
        with LOG_LOCK, open(LOG, "a") as f:
            f.write(line + "\n")


def ensure_cert() -> tuple[str, str]:
    os.makedirs(CERT_DIR, exist_ok=True)
    cert, key = (os.path.join(CERT_DIR, n) for n in ("cert.pem", "key.pem"))
    if not (os.path.exists(cert) and os.path.exists(key)):
        print("generating a self-signed certificate (openssl)...")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048",
                        "-keyout", key, "-out", cert, "-days", "3650",
                        "-nodes", "-subj", "/CN=testserver"],
                       check=True, capture_output=True)
    return cert, key


def main() -> int:
    global ROOT
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--http-port", type=int, default=PORTS["http"])
    ap.add_argument("--https-port", type=int, default=PORTS["https"])
    args = ap.parse_args()
    ROOT = os.path.abspath(args.root)
    PORTS.update(http=args.http_port, https=args.https_port)
    os.makedirs(ROOT, exist_ok=True)
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    cert, key = ensure_cert()
    print(f"file.bin md5 {hashlib.md5(PAYLOAD).hexdigest()} (65536 bytes); "
          f"serving {ROOT}")
    httpd = ThreadingHTTPServer(("0.0.0.0", args.http_port), Handler)
    httpsd = ThreadingHTTPServer(("0.0.0.0", args.https_port), Handler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    httpsd.socket = ctx.wrap_socket(httpsd.socket, server_side=True)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    print(f"http on port {args.http_port}, https on port {args.https_port}",
          flush=True)
    httpsd.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
