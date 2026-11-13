import tempfile
import unittest
from pathlib import Path

from src.feedback import FeedbackController
from src.protocol import Control, Kind, ProtocolError, decode_frame, encode_control, encode_message
from src.store import MessageStore


class FeedbackTests(unittest.TestCase):
    """Verify feedback behavior using isolated local fixtures.

    Args:
        *args (object): Arguments supplied by the unittest or HTTP server framework.
        **kwargs (object): Keyword options forwarded to the inherited framework constructor.

    Returns:
        FeedbackTests: Framework-managed test fixture.
    """

    def setUp(self) -> None:
        """Create a fresh temporary SQLite database and test fixtures for each case.

        Args:
            None.

        Returns:
            None: Test fixture attributes are initialized.
        """
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name) / "router.sqlite3"
        self.store = MessageStore(self.path)
        self.ctrl = FeedbackController(self.store, quiet_seconds=0)

    def tearDown(self) -> None:
        """Close the test database and remove the temporary directory.

        Args:
            None.

        Returns:
            None: Test resources are released.
        """
        self.store.close()
        self.dir.cleanup()

    def frames(self, *, data: bytes = b"image-data" * 50, message_id: int = 3):
        """Build image DATA frames and a matching END for feedback scenarios.

        Args:
            data (bytes): Application payload to fragment.
            message_id (int): Identifier shared by DATA and END.

        Returns:
            tuple[list[bytes], bytes]: Ordered DATA frames and encoded END.
        """
        parts = encode_message(Kind.IMAGE, 2, message_id, data, part_size=100)
        end = encode_control(Kind.END, decode_frame(parts[0]))
        return parts, end

    def reply(self) -> Control | None:
        """Decode pending feedback after the simulated receive quiet period.

        Args:
            None.

        Returns:
            Control | None: Decoded response, or None when no END reply is pending.
        """
        pending = self.ctrl.poll(now=999999)
        return decode_frame(pending.packet) if pending else None

    def test_no_ack_or_nack_without_end(self) -> None:
        """Verify no ack or nack without end.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        parts, end = self.frames()
        for part in parts:
            self.ctrl.on_frame(part, now=1)
        self.assertIsNone(self.reply())
        self.assertIsNone(self.ctrl.poll(now=100000000))
        self.ctrl.on_frame(end, now=2)
        reply = self.reply()
        self.assertEqual(reply.kind, Kind.ACK)
        self.assertEqual(reply.for_kind, Kind.IMAGE)

    def test_missing_fragment_nack_only_after_end(self) -> None:
        """Verify missing fragment nack only after end.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        parts, end = self.frames()
        missing_index = 2
        for i, frame in enumerate(parts):
            if i != missing_index:
                self.ctrl.on_frame(frame, now=1)
        self.assertIsNone(self.reply())
        self.ctrl.on_frame(end, now=2)
        response = self.reply()
        self.assertEqual(response.kind, Kind.NACK)
        self.assertEqual(response.missing, (2,))
        self.ctrl.complete_attempt(self.ctrl.poll(now=999999))
        self.assertIsNone(self.reply())  # no repeating NACK without new END
        self.ctrl.on_frame(parts[missing_index], now=3)
        self.assertIsNone(self.reply())  # not an ACK for retransmitted fragment
        self.ctrl.on_frame(end, now=4)
        self.assertEqual(self.reply().kind, Kind.ACK)

    def test_last_fragment_lost(self) -> None:
        """Verify last fragment lost.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        parts, end = self.frames()
        for part in parts[:-1]:
            self.ctrl.on_frame(part, now=1)
        self.ctrl.on_frame(end, now=2)
        self.assertEqual(self.reply().missing, (len(parts) - 1,))

    def test_every_fragment_lost_end_alone(self) -> None:
        """Verify every fragment lost end alone.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        parts, end = self.frames()
        self.ctrl.on_frame(end, now=1)
        response = self.reply()
        self.assertEqual(response.kind, Kind.NACK)
        self.assertEqual(response.missing, tuple(range(len(parts))))

    def test_retry_end_after_lost_ack_and_restart(self) -> None:
        """Verify retry end after lost ack and restart.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        parts, end = self.frames()
        for part in parts:
            self.ctrl.on_frame(part, now=1)
        self.ctrl.on_frame(end, now=2)
        self.assertEqual(self.reply().kind, Kind.ACK)
        self.ctrl.complete_attempt(self.ctrl.poll(now=999999))
        self.store.close()
        self.store = MessageStore(self.path)
        self.ctrl = FeedbackController(self.store, quiet_seconds=0)
        self.assertIsNone(self.reply())
        self.ctrl.on_frame(end, now=3)
        self.assertEqual(self.reply().kind, Kind.ACK)

    def test_nack_paginates_at_32_after_repeated_end(self) -> None:
        """Verify nack paginates at 32 after repeated end.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        parts, end = self.frames(data=b"a" * 8000)
        self.ctrl.on_frame(end, now=1)
        self.assertEqual(self.reply().missing, tuple(range(32)))
        self.ctrl.complete_attempt(self.ctrl.poll(now=999999))
        self.assertIsNone(self.reply())
        for part in parts[:32]:
            self.ctrl.on_frame(part, now=2)
        self.ctrl.on_frame(end, now=3)
        self.assertEqual(self.reply().missing, tuple(range(32, 64)))

    def test_invalid_or_mismatched_end_does_not_ack(self) -> None:
        """Verify invalid or mismatched end does not ack.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        parts, end = self.frames()
        self.ctrl.on_frame(parts[0], now=1)
        conflicting = bytearray(end)
        conflicting[12] ^= 1  # changes count metadata
        with self.assertRaises(ProtocolError):
            self.ctrl.on_frame(bytes(conflicting), now=2)
        self.assertIsNone(self.reply())

    def test_airtime_persisted(self) -> None:
        """Verify airtime persisted.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        parts, end = self.frames()
        self.ctrl.on_frame(end, now=1)
        msg = self.ctrl.poll(now=999999)
        self.assertIsNotNone(msg)
        self.assertTrue(self.ctrl.reserve_transmit(msg))
        self.assertFalse(self.ctrl.reserve_transmit(msg))
        self.store.close()
        self.store = MessageStore(self.path)
        self.ctrl = FeedbackController(self.store, quiet_seconds=0)
        self.ctrl.on_frame(end, now=1)
        msg = self.ctrl.poll(now=999999)
        self.assertFalse(self.ctrl.reserve_transmit(msg))


if __name__ == "__main__":
    unittest.main()
