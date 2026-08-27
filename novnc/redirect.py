"""Redirect plain http:// to https:// so operators can type a bare address.

Run as a second container from this same image:

    command: ["python3", "/usr/local/bin/redirect.py"]

noVNC needs a secure context — window.crypto.subtle is undefined over plain
http, and the RA2ne handshake dies on it — so http can only ever be a signpost
to https, never a fallback.
"""
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(os.environ.get("REDIRECT_PORT", "80"))
TARGET_PORT = os.environ.get("REDIRECT_TO_PORT", "443")


class Redirect(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self):
        # Host may carry a port; keep only the name and use the https port.
        host = (self.headers.get("Host") or "").split(":")[0]
        if not host:
            self.send_error(400, "Missing Host header")
            return
        target = f"https://{host}" if TARGET_PORT == "443" else f"https://{host}:{TARGET_PORT}"
        self.send_response(301)
        self.send_header("Location", target + self.path)
        self.send_header("Content-Length", "0")
        self.end_headers()

    do_GET = do_HEAD = _send

    def log_message(self, fmt, *args):
        pass  # one line per probe is noise on a station


if __name__ == "__main__":
    print(f"redirect: http://:{PORT} -> https (port {TARGET_PORT})", flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), Redirect).serve_forever()
