"""Minimal local HTTP server for integration testing; not production backend.

Run: python dev_receiver.py --port 8765
Saves bodies into ./dev_received/ with a unique filename.
"""

import argparse
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class Handler(BaseHTTPRequestHandler):
    """Save supported POST bodies to disk for local integration checks. This server has no backend
    deduplication.

    Args:
        *args (object): Request socket, client address and server supplied by HTTPServer.
        **kwargs (object): Additional BaseHTTPRequestHandler initialization options.

    Returns:
        Handler: Development HTTP request handler.
    """

    def do_POST(self) -> None:
        """Validate Content-Length, save the request body and reply with HTTP 201.

        Args:
            None.

        Returns:
            None: This operation returns no value.
        """
        if self.path not in ("/api/events", "/api/images", "/api/telemetry"):
            self.send_error(404)
            return
        try:
            size = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.send_error(400, "Content-Length must be an integer")
            return
        if not 0 < size <= 2_000_000:
            self.send_error(413)
            return
        content = self.rfile.read(size)
        out = Path("dev_received")
        out.mkdir(exist_ok=True)
        suffix = ".multipart" if self.path == "/api/images" else ".json"
        target = out / (self.path.split("/")[-1] + "-" + uuid.uuid4().hex + suffix)
        target.write_bytes(content)
        print("POST", self.path, "=>", target, flush=True)
        self.send_response(201)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{"ok":true}')


def main() -> None:
    """Parse the development receiver port and serve on IPv4 loopback until interrupted.

    Args:
        None.

    Returns:
        None: This operation returns no value.
    """
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"Local development receiver: http://127.0.0.1:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
