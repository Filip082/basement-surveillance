"""Forward complete messages to configurable, local HTTP endpoints."""

import json
import math
import uuid
from urllib.parse import urlparse
from urllib.request import Request, urlopen

from .protocol import Kind, ProtocolError
from .store import ReadyMessage


class HttpForwarder:
    """Send complete persisted messages to backend endpoints with stable idempotency keys.

    Args:
        base_url (str): HTTP(S) backend origin, including an optional port.
        timeout (float): Positive socket timeout in seconds for each HTTP request.
        event_path (str): Absolute URL path receiving entry-event JSON.
        image_path (str): Absolute URL path receiving JPEG multipart uploads.
        telemetry_path (str): Absolute URL path receiving telemetry JSON.

    Returns:
        HttpForwarder: HTTP sender with validated origin and endpoint paths.
    """

    def __init__(
        self,
        base_url: str = "http://127.0.0.1:8000",
        timeout: float = 5,
        event_path: str = "/api/events",
        image_path: str = "/api/images",
        telemetry_path: str = "/api/telemetry",
    ) -> None:
        """Validate the backend origin, timeout and endpoint paths before forwarding.

        Args:
            base_url (str): HTTP(S) backend origin, including an optional port.
            timeout (float): Positive socket timeout in seconds for each HTTP request.
            event_path (str): Absolute URL path receiving entry-event JSON.
            image_path (str): Absolute URL path receiving JPEG multipart uploads.
            telemetry_path (str): Absolute URL path receiving telemetry JSON.

        Returns:
            None: This operation returns no value.
        """
        base_url = base_url.rstrip("/")
        parsed = urlparse(base_url)
        if (
            parsed.scheme not in ("http", "https")
            or not parsed.hostname
            or parsed.username is not None
            or parsed.password is not None
            or parsed.path not in ("", "/")
            or parsed.query
            or parsed.fragment
        ):
            raise ValueError("backend URL must be an HTTP(S) origin without credentials or path")
        if parsed.port is not None and not 1 <= parsed.port <= 65535:
            raise ValueError("backend port must be in 1..65535")
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("HTTP timeout must be positive and finite")
        for path in (event_path, image_path, telemetry_path):
            if not path.startswith("/") or "?" in path or "#" in path:
                raise ValueError("endpoint paths must begin with / and have no query")
        self.base_url = base_url
        self.timeout = timeout
        self.paths = {
            Kind.ENTRY: event_path,
            Kind.IMAGE: image_path,
            Kind.TELEMETRY: telemetry_path,
        }

    def deliver(self, msg: ReadyMessage) -> None:
        """POST JSON or JPEG multipart data. Backend errors propagate so the outbox can retry.

        Args:
            msg (ReadyMessage): Complete stored message with payload and reception metadata.

        Returns:
            None: This operation returns no value.
        """
        fields = {
            "device_id": msg.device_id,
            "message_id": msg.message_id,
            "received_at": msg.received_at,
            "rssi": msg.rssi,
            "snr": msg.snr,
        }
        headers = {"Idempotency-Key": msg.idempotency_key}
        if msg.kind == Kind.IMAGE:
            body, content_type = self._multipart(fields, msg.body)
        else:
            try:
                payload = json.loads(msg.body.decode("utf-8"))
            except (ValueError, UnicodeDecodeError) as exc:
                raise ProtocolError("invalid JSON payload") from exc
            if not isinstance(payload, dict):
                raise ProtocolError("JSON message must be an object")
            body = json.dumps({**fields, "payload": payload}, separators=(",", ":")).encode("utf-8")
            content_type = "application/json"
        headers["Content-Type"] = content_type
        req = Request(
            self.base_url + self.paths[msg.kind], data=body, headers=headers, method="POST"
        )
        with urlopen(req, timeout=self.timeout) as res:
            if not 200 <= res.status < 300:
                raise RuntimeError(f"HTTP {res.status}")

    @staticmethod
    def _multipart(fields: dict, photo: bytes) -> tuple[bytes, str]:
        """Build a JPEG multipart body with scalar metadata and a unique MIME boundary.

        Args:
            fields (dict): Metadata fields; entries with None values are omitted.
            photo (bytes): Raw JPEG file contents.

        Returns:
            tuple[bytes, str]: Encoded request body and its multipart Content-Type header.
        """
        boundary = "bs-" + uuid.uuid4().hex
        buf = bytearray()
        for name, value in fields.items():
            if value is None:
                continue
            buf += (
                f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n'
            ).encode("ascii")
        buf += (
            f"--{boundary}\r\nContent-Disposition: form-data; "
            'name="file"; filename="snapshot.jpg"\r\n'
            "Content-Type: image/jpeg\r\n\r\n"
        ).encode("ascii")
        buf += photo
        buf += f"\r\n--{boundary}--\r\n".encode("ascii")
        return bytes(buf), f"multipart/form-data; boundary={boundary}"
