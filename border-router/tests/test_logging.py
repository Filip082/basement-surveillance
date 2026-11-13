"""Verify backend-independent receive diagnostics and radio-only operation."""

import io
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import MagicMock, patch

from src.feedback import FeedbackController
from src.main import run
from src.protocol import Kind, decode_frame, encode_control, encode_message
from src.radio import RadioConfig, SX1262Transceiver
from src.store import MessageStore


class ReceiveLoggingTests(unittest.TestCase):
    """Exercise reception without requiring GPIO hardware or a backend service.

    Args:
        *args (object): Positional arguments passed to unittest.TestCase.
        **kwargs (object): Keyword arguments passed to unittest.TestCase.

    Returns:
        ReceiveLoggingTests: Framework-managed regression fixture.
    """

    def test_fragment_end_and_complete_message_are_visible_at_info(self) -> None:
        """Verify partial reception and complete durable receipt have distinct INFO logs.

        Args:
            None.

        Returns:
            None: Assertions check fragment identity, full body size and queued feedback.
        """
        with tempfile.TemporaryDirectory() as directory:
            store = MessageStore(Path(directory) / "router.sqlite3")
            try:
                controller = FeedbackController(store, quiet_seconds=0)
                parts = encode_message(Kind.IMAGE, 7, 42, b"abcdefgh", part_size=4)
                end = encode_control(Kind.END, decode_frame(parts[0]))
                with self.assertLogs("src.feedback", level="INFO") as captured:
                    self.assertFalse(controller.on_frame(parts[0]))
                    self.assertTrue(controller.on_frame(parts[1]))
                    controller.on_frame(end)
                logs = "\n".join(captured.output)
                self.assertIn("device=7 kind=IMAGE id=42 part=1/2 payload_bytes=4", logs)
                self.assertIn("part=2/2", logs)
                self.assertIn("message complete device=7 kind=IMAGE id=42 bytes=8", logs)
                self.assertIn("LoRa END device=7 kind=IMAGE id=42 parts=2", logs)
                self.assertEqual(controller.poll().kind, Kind.ACK)
            finally:
                store.close()

    def test_no_http_preserves_ack_and_outbox_without_creating_a_worker(self) -> None:
        """Verify radio-only mode performs no HTTP setup and retains messages for later delivery.

        Args:
            None.

        Returns:
            None: Assertions check ACK output, durable storage and absent HTTP activity.
        """
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "router.sqlite3"
            parts = encode_message(Kind.ENTRY, 1, 2, b'{"test":true}')
            end = encode_control(Kind.END, decode_frame(parts[0]))
            stdin = io.StringIO("\n".join(frame.hex() for frame in [*parts, end]) + "\n")
            stdout = io.StringIO()
            with (
                patch("src.main.HttpForwarder") as forwarder,
                patch("src.main.Thread") as worker,
                patch("src.main.signal.signal"),
                patch("src.main.sys.stdin", stdin),
                redirect_stdout(stdout),
                self.assertLogs(level="INFO") as captured,
            ):
                status = run(
                    [
                        "--simulate-hex",
                        "--no-http",
                        "--backend-url",
                        "invalid-backend-unused-in-radio-only-mode",
                        "--db",
                        str(path),
                    ]
                )
            self.assertEqual(status, 0)
            forwarder.assert_not_called()
            worker.assert_not_called()
            self.assertIn("ACK:", stdout.getvalue())
            self.assertIn("HTTP forwarding disabled", "\n".join(captured.output))
            store = MessageStore(path)
            try:
                self.assertEqual(store.ready()[0].body, b'{"test":true}')
            finally:
                store.close()

    def test_raw_radio_reception_is_logged_even_for_an_invalid_protocol_frame(self) -> None:
        """Verify PHY reception remains diagnosable when the sender's application framing is wrong.

        Args:
            None.

        Returns:
            None: Assertions distinguish signal reception from protocol rejection.
        """
        radio = MagicMock()
        radio.begin.return_value = True
        radio.request.return_value = True
        radio.getIrqStatus.return_value = 1
        radio.wait.return_value = True
        radio.status.return_value = radio.STATUS_RX_DONE
        radio.available.return_value = 3
        radio.get.return_value = b"bad"
        radio.packetRssi.return_value = -90
        radio.snr.return_value = 7.25
        driver = MagicMock()
        driver.SX126x.return_value = radio
        modules = {"gpiod": MagicMock(), "gpiod.line": MagicMock(), "LoRaRF": driver}
        with tempfile.TemporaryDirectory() as directory:
            store = MessageStore(Path(directory) / "router.sqlite3")
            try:
                controller = FeedbackController(store)
                with (
                    patch.dict("sys.modules", modules),
                    self.assertLogs("src.radio", level="INFO") as captured,
                ):
                    SX1262Transceiver(RadioConfig()).run(
                        MagicMock(side_effect=(False, True)), controller
                    )
                logs = "\n".join(captured.output)
                self.assertIn("LoRa RX bytes=3 rssi=-90 dBm snr=7.25 dB", logs)
                self.assertIn("Rejected received frame", logs)
            finally:
                store.close()
