import json
import tempfile
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread

from src.http_client import HttpForwarder
from src.protocol import Kind, ProtocolError, decode_frame, encode_message
from src.store import MessageStore


class Handler(BaseHTTPRequestHandler):
    """Capture backend requests for HTTP forwarding assertions.

    Args:
        *args (object): Arguments supplied by the unittest or HTTP server framework.
        **kwargs (object): Keyword options forwarded to the inherited framework constructor.

    Returns:
        Handler: Framework-managed test fixture.
    """

    requests = []

    def do_POST(self) -> None:
        """Capture request path, headers and body, then report successful delivery.

        Args:
            None.

        Returns:
            None: The HTTP response is written to the client.
        """
        size = int(self.headers["Content-Length"])
        body = self.rfile.read(size)
        self.requests.append((self.path, dict(self.headers), body))
        self.send_response(201)
        self.end_headers()

    def log_message(self, *_) -> None:
        """Suppress HTTP access logs during unit tests.

        Args:
            *_ (object): Logging format and values supplied by BaseHTTPRequestHandler.

        Returns:
            None: Logging is intentionally suppressed.
        """
        pass


class RouterTests(unittest.TestCase):
    """Verify router behavior using isolated local fixtures.

    Args:
        *args (object): Arguments supplied by the unittest or HTTP server framework.
        **kwargs (object): Keyword options forwarded to the inherited framework constructor.

    Returns:
        RouterTests: Framework-managed test fixture.
    """

    def setUp(self) -> None:
        """Create a fresh temporary SQLite database and test fixtures for each case.

        Args:
            None.

        Returns:
            None: Test fixture attributes are initialized.
        """
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "router.db"
        self.store = MessageStore(self.path)

    def tearDown(self) -> None:
        """Close the test database and remove the temporary directory.

        Args:
            None.

        Returns:
            None: Test resources are released.
        """
        self.store.close()
        self.temp.cleanup()

    def test_image_out_of_order_duplicate_and_restart(self) -> None:
        """Verify image out of order duplicate and restart.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        data = b"\xff\xd8" + bytes(range(256)) * 10 + b"\xff\xd9"
        packets = encode_message(Kind.IMAGE, 7, 101, data, part_size=100)
        frames = [decode_frame(p) for p in packets]
        self.assertFalse(self.store.accept(frames[-1]))
        self.assertFalse(self.store.accept(frames[-1]))
        self.store.close()
        self.store = MessageStore(self.path)
        for frame in frames[1:-1]:
            self.assertFalse(self.store.accept(frame))
        self.assertTrue(self.store.accept(frames[0]))
        self.assertEqual(self.store.ready()[0].body, data)
        self.assertFalse(self.store.accept(frames[0]))
        self.store.mark_sent(self.store.ready()[0])
        self.assertEqual(self.store.ready(), [])

    def test_crc_rejected(self) -> None:
        """Verify crc rejected.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        packets = encode_message(Kind.IMAGE, 1, 10, b"abcdefgh", part_size=4)
        self.assertFalse(self.store.accept(decode_frame(packets[0])))
        tampered = bytearray(packets[1])
        tampered[-1] ^= 0x80
        with self.assertRaises(ProtocolError):
            self.store.accept(decode_frame(bytes(tampered)))
        self.assertEqual(self.store.ready(), [])

    def test_http_dispatch(self) -> None:
        """Verify http dispatch.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        Handler.requests = []
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            fwd = HttpForwarder(f"http://127.0.0.1:{server.server_port}")
            data = {"event": "person_entered", "timestamp": "2026-11-06T10:00:00Z"}
            for kind, ident, body in [
                (Kind.ENTRY, 1, json.dumps(data).encode()),
                (Kind.IMAGE, 1, b"\xff\xd8image\xff\xd9"),
                (Kind.TELEMETRY, 2, b'{"battery_mv": 3750}'),
            ]:
                for frame in encode_message(kind, 9, ident, body):
                    self.store.accept(decode_frame(frame))
            for msg in self.store.ready():
                fwd.deliver(msg)
                self.store.mark_sent(msg)
            self.assertEqual(
                [r[0] for r in Handler.requests], ["/api/events", "/api/images", "/api/telemetry"]
            )
            body = json.loads(Handler.requests[0][2])
            self.assertEqual(body["payload"], data)
            self.assertEqual(body["device_id"], 9)
            self.assertIn(b"\xff\xd8image\xff\xd9", Handler.requests[1][2])
            self.assertIn("Idempotency-Key", Handler.requests[1][1])
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()
