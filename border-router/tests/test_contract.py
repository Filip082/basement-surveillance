"""Regression coverage for version 1 framing, radio settings and airtime controls."""

import sqlite3
import tempfile
import unittest
from contextlib import closing
from pathlib import Path
from unittest.mock import MagicMock, patch

from src.feedback import FeedbackController, lora_airtime_seconds
from src.http_client import HttpForwarder
from src.protocol import VERSION, Kind, ProtocolError, decode_frame, encode_control, encode_message
from src.radio import RadioConfig, SX1262Transceiver
from src.store import MessageStore


class ContractTests(unittest.TestCase):
    """Verify contract behavior using isolated local fixtures.

    Args:
        *args (object): Arguments supplied by the unittest or HTTP server framework.
        **kwargs (object): Keyword options forwarded to the inherited framework constructor.

    Returns:
        ContractTests: Framework-managed test fixture.
    """

    def test_version_one_and_old_wire_version_rejected(self) -> None:
        """Verify version one and old wire version rejected.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        frame = encode_message(Kind.ENTRY, 1, 2, b"{}")[0]
        self.assertEqual(VERSION, 1)
        self.assertEqual(frame[2], 1)
        for version in (0, 2, 3):
            with self.subTest(version=version):
                changed = bytearray(frame)
                changed[2] = version
                with self.assertRaises(ProtocolError):
                    decode_frame(bytes(changed))

    def test_failed_outbox_batch_defers_without_starving_new_messages(self) -> None:
        """Verify persisted retry delays allow later outbox messages to be delivered.

        Args:
            None.

        Returns:
            None: Assertions check batch fairness and recovery after restart and retry time.
        """
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "router.sqlite3"
            store = MessageStore(path)
            try:
                for message_id in range(11):
                    store.accept(decode_frame(encode_message(Kind.ENTRY, 1, message_id, b"{}")[0]))
                with patch("src.store.time.time", return_value=100):
                    for item in store.ready():
                        store.retry_later(item)
                    self.assertEqual([item.message_id for item in store.ready()], [10])
                store.close()
                store = MessageStore(path)
                with patch("src.store.time.time", return_value=104):
                    self.assertEqual([item.message_id for item in store.ready()], [10])
                with patch("src.store.time.time", return_value=106):
                    self.assertEqual([item.message_id for item in store.ready()], list(range(10)))
            finally:
                store.close()

    def test_nack_requires_sorted_unique_in_range_indices(self) -> None:
        """Verify nack requires sorted unique in range indices.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        original = decode_frame(encode_message(Kind.IMAGE, 1, 1, b"abcd", part_size=1)[0])
        for missing in ((), (1, 1), (2, 1), (4,), (-1,)):
            with self.subTest(missing=missing), self.assertRaises(ValueError):
                encode_control(Kind.NACK, original, missing)
        valid = encode_control(Kind.NACK, original, (0, 2))
        self.assertEqual(decode_frame(valid).missing, (0, 2))
        with self.assertRaises(ProtocolError):
            decode_frame(valid[:-1])

    def test_crc_failure_recovers_on_end_with_same_message_id(self) -> None:
        """Verify crc failure recovers on end with same message id.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        with tempfile.TemporaryDirectory() as directory:
            store = MessageStore(Path(directory) / "router.sqlite3")
            self.addCleanup(store.close)
            controller = FeedbackController(store, quiet_seconds=0)
            parts = encode_message(Kind.IMAGE, 1, 1, b"abcdef", part_size=3)
            end = encode_control(Kind.END, decode_frame(parts[0]))
            controller.on_frame(parts[0])
            corrupt = parts[1][:-1] + bytes([parts[1][-1] ^ 1])
            with self.assertRaises(ProtocolError):
                controller.on_frame(corrupt)
            controller.on_frame(end)
            response = controller.poll()
            self.assertEqual(decode_frame(response.packet).missing, (0, 1))
            controller.complete_attempt(response)
            for part in parts:
                controller.on_frame(part)
            self.assertIsNone(controller.poll())
            controller.on_frame(end)
            self.assertEqual(controller.poll().kind, Kind.ACK)

    def test_schema_and_idempotency_are_version_one(self) -> None:
        """Verify schema and idempotency are version one.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "router.sqlite3"
            store = MessageStore(path)
            try:
                self.assertEqual(store.db.execute("PRAGMA user_version").fetchone()[0], 1)
                frame = decode_frame(encode_message(Kind.ENTRY, 7, 9, b"{}")[0])
                store.accept(frame)
                self.assertEqual(store.ready()[0].idempotency_key, "bs-v1:7:1:9")
            finally:
                store.close()
            with closing(sqlite3.connect(path)) as db:
                db.execute("PRAGMA user_version=3")
            with self.assertRaisesRegex(RuntimeError, "fresh router-v1"):
                MessageStore(path)

    def test_backend_origins_support_compose_and_https(self) -> None:
        """Verify backend origins support compose and https.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        for origin in ("http://backend:8000", "https://example.com", "http://[::1]:8000"):
            with self.subTest(origin=origin):
                self.assertEqual(HttpForwarder(origin).base_url, origin)
        for origin in (
            "ftp://backend",
            "http://user:pass@backend",
            "http://backend/path",
            "http://backend?query=1",
            "http://backend:0",
        ):
            with self.subTest(origin=origin), self.assertRaises(ValueError):
                HttpForwarder(origin)

    def test_radio_rejects_silent_driver_clamping_and_pin_conflicts(self) -> None:
        """Verify radio rejects silent driver clamping and pin conflicts.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        for kwargs in (
            {"bandwidth": 123456},
            {"spreading_factor": 13},
            {"coding_rate": 4},
            {"tx_power": 15},
            {"reset_gpio": 8},
            {"reset_gpio": 23},
            {"sync_word": 0x12},
            {"poll_interval": float("nan")},
        ):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                RadioConfig(**kwargs)

    def test_airtime_reference_values(self) -> None:
        """Verify airtime reference values.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        self.assertAlmostEqual(lora_airtime_seconds(23, sf=5, bw=125000, cr=5), 0.019520)
        self.assertAlmostEqual(lora_airtime_seconds(23, sf=6, bw=125000, cr=5), 0.036480)
        self.assertAlmostEqual(lora_airtime_seconds(23, sf=7, bw=125000, cr=5), 0.065792)
        self.assertAlmostEqual(lora_airtime_seconds(23, sf=12, bw=125000, cr=5), 1.613824)

    def test_airtime_cooldown_and_rolling_window(self) -> None:
        """Verify airtime cooldown and rolling window.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        with tempfile.TemporaryDirectory() as directory:
            store = MessageStore(Path(directory) / "router.sqlite3")
            try:
                with patch("src.store.time.time", return_value=100):
                    self.assertTrue(store.reserve_airtime(1, fraction=0.01, window=100))
                with patch("src.store.time.time", return_value=199):
                    self.assertFalse(store.reserve_airtime(1, fraction=0.01, window=100))
                with patch("src.store.time.time", return_value=201):
                    self.assertTrue(store.reserve_airtime(1, fraction=0.01, window=100))
                for duration, fraction, window in (
                    (float("nan"), 0.01, 3600),
                    (1, float("nan"), 3600),
                    (1, 0.01, 0),
                ):
                    with self.subTest(duration=duration, fraction=fraction, window=window):
                        with self.assertRaises(ValueError):
                            store.reserve_airtime(duration, fraction=fraction, window=window)
            finally:
                store.close()

    def test_disabled_limiter_bypasses_persisted_budget_and_off_time(self) -> None:
        """Verify disabled limiter bypasses persisted budget and off time.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        with tempfile.TemporaryDirectory() as directory:
            store = MessageStore(Path(directory) / "router.sqlite3")
            try:
                controller = FeedbackController(store, quiet_seconds=0)
                frame = decode_frame(encode_message(Kind.ENTRY, 1, 1, b"{}")[0])
                controller.on_frame(encode_control(Kind.END, frame))
                feedback = controller.poll()
                self.assertTrue(controller.reserve_transmit(feedback))
                self.assertFalse(controller.reserve_transmit(feedback))
                unrestricted = FeedbackController(store, duty_cycle=0)
                with patch.object(
                    store, "reserve_airtime", side_effect=AssertionError("must bypass")
                ):
                    self.assertTrue(unrestricted.reserve_transmit(feedback))
                    self.assertTrue(unrestricted.reserve_transmit(feedback))
                for duty_cycle in (-1, float("nan"), 1.01):
                    with self.subTest(duty_cycle=duty_cycle), self.assertRaises(ValueError):
                        FeedbackController(store, duty_cycle=duty_cycle)
            finally:
                store.close()

    def test_radio_slow_symbol_setup_and_timeout_returns_to_standby(self) -> None:
        """Verify radio slow symbol setup and timeout returns to standby.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        radio = MagicMock()
        radio.begin.return_value = True
        radio.request.return_value = True
        radio.getIrqStatus.return_value = 0
        radio.endPacket.return_value = True
        radio.wait.return_value = False
        gpiod = MagicMock()
        line = MagicMock()
        driver = MagicMock()
        driver.SX126x.return_value = radio
        controller = MagicMock()
        controller.sf, controller.bw, controller.cr = 12, 125000, 5
        controller.poll.return_value.packet = b"x" * 23
        controller.reserve_transmit.return_value = True
        modules = {"gpiod": gpiod, "gpiod.line": line, "LoRaRF": driver}
        with patch.dict("sys.modules", modules), self.assertLogs("src.radio", level="ERROR"):
            SX1262Transceiver(RadioConfig(spreading_factor=12)).run(
                MagicMock(side_effect=(False, True)), controller
            )
        radio.setLoRaModulation.assert_called_once_with(12, 125000, 5, True)
        radio.setTxPower.assert_called_once_with(14, radio.TX_POWER_SX1262)
        radio.setTxParams.assert_not_called()
        self.assertEqual(radio.setStandby.call_count, 2)
        controller.complete_attempt.assert_called_once()
        radio.end.assert_called_once()
        gpiod.request_lines.return_value.release.assert_called_once()


if __name__ == "__main__":
    unittest.main()
