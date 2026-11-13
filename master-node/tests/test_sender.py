"""Compile the portable C sender and exercise it against the actual Python router.

Run from master-node/: python3.15 -m unittest discover -s tests -v
Requires a C11 host compiler and the sibling border-router/ source, not LoRa hardware.
"""

import ctypes as C
import errno
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROUTER = Path(os.getenv("BORDER_ROUTER_PATH", str(ROOT.parent / "border-router"))).resolve()
sys.path.insert(0, str(ROUTER))
from src.feedback import FeedbackController
from src.protocol import Control, Fragment, Kind, decode_frame, encode_control, encode_message
from src.store import MessageStore


class Message(C.Structure):
    """Mirror C metadata for exercising the public portable protocol functions.

    Args:
        None.

    Returns:
        Message: Initialized test fixture or C-compatible structure.
    """

    _fields_ = [
        ("message_id", C.c_uint32),
        ("total_size", C.c_uint32),
        ("crc32", C.c_uint32),
        ("device_id", C.c_uint16),
        ("count", C.c_uint16),
        ("part_size", C.c_uint16),
        ("kind", C.c_uint8),
    ]


class Feedback(C.Structure):
    """Mirror the C decoder's bounded ACK/NACK output.

    Args:
        None.

    Returns:
        Feedback: Initialized test fixture or C-compatible structure.
    """

    _fields_ = [("missing", C.c_uint16 * 32), ("missing_count", C.c_uint8), ("kind", C.c_uint8)]


class Options(C.Structure):
    """Configure retry limits and logical receive timing for the portable state machine.

    Args:
        None.

    Returns:
        Options: Initialized test fixture or C-compatible structure.
    """

    _fields_ = [
        ("feedback_timeout_ms", C.c_uint32),
        ("part_size", C.c_uint16),
        ("max_end_attempts", C.c_uint16),
        ("max_packet_attempts", C.c_uint8),
    ]


class Result(C.Structure):
    """Expose DATA/END attempts and ignored feedback without depending on logs.

    Args:
        None.

    Returns:
        Result: Initialized test fixture or C-compatible structure.
    """

    _fields_ = [
        ("data_attempts", C.c_uint32),
        ("end_attempts", C.c_uint32),
        ("ignored_feedback", C.c_uint32),
        ("fragment_count", C.c_uint16),
    ]


Send = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(C.c_uint8), C.c_size_t)
Receive = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(C.c_uint8), C.c_size_t, C.c_uint32)
Now = C.CFUNCTYPE(C.c_uint64, C.c_void_p)
Event = C.CFUNCTYPE(None, C.c_void_p, C.c_int, C.c_int)


class IO(C.Structure):
    """Bind C state-machine callbacks to a deterministic Python receiver simulation.

    Args:
        None.

    Returns:
        IO: Initialized test fixture or C-compatible structure.
    """

    _fields_ = [
        ("context", C.c_void_p),
        ("send", Send),
        ("recv", Receive),
        ("now_ms", Now),
        ("event", Event),
    ]


def buffer(data):
    """Return a ctypes byte buffer that keeps binary payloads, including NULs, intact.

    Args:
        data (bytes): Application or golden-vector bytes, including embedded NUL values.

    Returns:
        ctypes array: Owned byte array suitable for a C pointer argument.
    """
    return (C.c_uint8 * len(data)).from_buffer_copy(data)


class RouterLink:
    """Simulate a half-duplex link and route C frames through real SQLite/feedback code.

    Args:
        directory (str | Path): Temporary directory containing an isolated receiver database.

    Returns:
        RouterLink: Initialized test fixture or C-compatible structure.
    """

    def __init__(self, directory):
        """Create a deterministic link connected to the actual Python receiver.

        Args:
            directory (str | Path): Temporary directory containing an isolated receiver database.

        Returns:
            None: Test fixtures are initialized or released.
        """
        self.store = MessageStore(Path(directory) / "router.sqlite3")
        self.controller = FeedbackController(self.store, quiet_seconds=0)
        self.sent = []
        self.time_ms = 0
        self.drop_parts = set()
        self.drop_end_count = 0
        self.drop_feedback_count = 0
        self.inject = []
        self.tx_failures = 0
        self.receive_error = 0
        self.receive_calls = 0
        self.callback_error = None
        self.callbacks = (
            Send(self.send),
            Receive(self.receive),
            Now(lambda _: self.time_ms),
            Event(),
        )
        self.io = IO(None, *self.callbacks)

    def send(self, context, raw, size):
        """Send binary frames through the configured loss/error simulation.

        Args:
            context (object): Opaque transport context; unused by this simulated link.
            raw (ctypes pointer): C packet bytes available during this callback.
            size (int): Exact number of packet or buffer bytes.

        Returns:
            int: Zero for an attempted simulated TX, or a negative radio error.
        """
        try:
            packet = C.string_at(raw, size)
            decoded = decode_frame(packet)
            self.sent.append(decoded)
            self.time_ms += 2
            if self.tx_failures:
                self.tx_failures -= 1
                return -errno.EIO
            if isinstance(decoded, Fragment) and decoded.index in self.drop_parts:
                self.drop_parts.remove(decoded.index)
                return 0
            if isinstance(decoded, Control) and self.drop_end_count:
                self.drop_end_count -= 1
                return 0
            self.controller.on_frame(packet, now=self.time_ms / 1000)
            return 0
        except Exception as exc:
            self.callback_error = exc
            return -errno.EIO

    def receive(self, context, output, capacity, timeout_ms):
        """Return injected or actual router feedback, or advance time on a timeout.

        Args:
            context (object): Opaque transport context; unused by this simulated link.
            output (ctypes pointer): C receive buffer to fill with a feedback frame.
            capacity (int): Maximum number of bytes that fit in the C receive buffer.
            timeout_ms (int): Remaining receive deadline in logical milliseconds.

        Returns:
            int: Feedback length, or a negative timeout/radio error.
        """
        self.receive_calls += 1
        if self.receive_error:
            return self.receive_error
        if self.inject:
            packet = self.inject.pop(0)
            self.time_ms += 1
        else:
            response = self.controller.poll(now=self.time_ms / 1000)
            if response is None:
                self.time_ms += timeout_ms
                return -errno.ETIMEDOUT
            self.controller.complete_attempt(response)
            if self.drop_feedback_count:
                self.drop_feedback_count -= 1
                self.time_ms += timeout_ms
                return -errno.ETIMEDOUT
            packet = response.packet
            self.time_ms += 1
        if len(packet) > capacity:
            return -errno.ENOBUFS
        C.memmove(output, packet, len(packet))
        return len(packet)


class SenderTests(unittest.TestCase):
    """Check byte-for-byte interoperability and bounded sender transitions without hardware.

    Args:
        None.

    Returns:
        SenderTests: Initialized test fixture or C-compatible structure.
    """

    @classmethod
    def setUpClass(cls):
        """Compile and load the real portable C sender using strict host compiler warnings.

        Args:
            None.

        Returns:
            None: Test fixtures are initialized or released.
        """
        cls.build = tempfile.TemporaryDirectory(prefix="bs-sender-tests-")
        library = Path(cls.build.name) / "sender.so"
        subprocess.run(
            [
                os.getenv("CC", "cc"),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                "-shared",
                "-fPIC",
                "-I",
                str(ROOT / "include"),
                str(ROOT / "src/protocol.c"),
                str(ROOT / "src/tx_session.c"),
                "-o",
                str(library),
            ],
            check=True,
        )
        cls.lib = C.CDLL(str(library))
        cls.lib.bs_tx_spacing_ms.argtypes = [C.c_uint32, C.c_uint16]
        cls.lib.bs_tx_spacing_ms.restype = C.c_uint64
        cls.lib.bs_crc32.argtypes = [C.c_void_p, C.c_size_t]
        cls.lib.bs_crc32.restype = C.c_uint32
        cls.lib.bs_message_init.argtypes = [
            C.POINTER(Message),
            C.c_uint8,
            C.c_uint16,
            C.c_uint32,
            C.c_void_p,
            C.c_size_t,
            C.c_uint16,
        ]
        cls.lib.bs_encode_data.argtypes = [
            C.POINTER(Message),
            C.c_void_p,
            C.c_size_t,
            C.c_uint16,
            C.c_void_p,
            C.c_size_t,
        ]
        cls.lib.bs_encode_end.argtypes = [C.POINTER(Message), C.c_void_p, C.c_size_t]
        cls.lib.bs_decode_feedback.argtypes = [
            C.POINTER(Message),
            C.c_void_p,
            C.c_size_t,
            C.POINTER(Feedback),
        ]
        cls.lib.bs_tx_send.argtypes = [
            C.POINTER(IO),
            C.POINTER(Options),
            C.c_uint8,
            C.c_uint16,
            C.c_uint32,
            C.c_void_p,
            C.c_size_t,
            C.POINTER(Result),
        ]

    @classmethod
    def tearDownClass(cls):
        """Remove the temporary host build artifacts.

        Args:
            None.

        Returns:
            None: Test fixtures are initialized or released.
        """
        cls.build.cleanup()

    def setUp(self):
        """Create an isolated link, SQLite store and bounded session options.

        Args:
            None.

        Returns:
            None: Test fixtures are initialized or released.
        """
        self.directory = tempfile.TemporaryDirectory()
        self.link = RouterLink(self.directory.name)
        self.options = Options(20000, 200, 8, 2)

    def tearDown(self):
        """Close the receiver database and remove the temporary fixtures.

        Args:
            None.

        Returns:
            None: Test fixtures are initialized or released.
        """
        self.link.store.close()
        self.directory.cleanup()

    def message(self, data=b"123456789", part_size=200):
        """Initialize valid C metadata for a byte-for-byte protocol test.

        Args:
            data (bytes): Application or golden-vector bytes, including embedded NUL values.
            part_size (int): Maximum DATA body bytes for the golden-vector scenario.

        Returns:
            Message: Validated C metadata, including CRC32 and fragment count.
        """
        result = Message()
        self.assertEqual(
            self.lib.bs_message_init(
                C.byref(result), Kind.IMAGE, 0x1234, 0x89ABCDEF, buffer(data), len(data), part_size
            ),
            0,
        )
        return result

    def send(self, data=b"a\x00b" * 200):
        """Run the real C sender against the fake link and check callback integrity.

        Args:
            data (bytes): Application or golden-vector bytes, including embedded NUL values.

        Returns:
            tuple[int, Result]: Sender status and DATA/END attempt counters.
        """
        result = Result()
        status = self.lib.bs_tx_send(
            C.byref(self.link.io),
            C.byref(self.options),
            Kind.IMAGE,
            0x1234,
            0x89ABCDEF,
            buffer(data),
            len(data),
            C.byref(result),
        )
        self.assertIsNone(self.link.callback_error)
        return status, result

    def test_duty_spacing_is_conservative_and_cannot_overflow_32_bits(self):
        """Verify integer duty spacing rounds upward and supports long boot guards.

        Args:
            None.

        Returns:
            None: Assertions cover default, strict, invalid and large timing inputs.
        """
        for airtime, duty in ((71, 10), (170, 3), (9000, 1), (0xFFFFFFFF, 1)):
            spacing = self.lib.bs_tx_spacing_ms(airtime, duty)
            self.assertGreaterEqual(spacing * duty, airtime * 1000)
            self.assertLess((spacing - 1) * duty, airtime * 1000)
            self.assertGreaterEqual(spacing, airtime)
        self.assertEqual(self.lib.bs_tx_spacing_ms(71, 10), 7100)
        for airtime, duty in ((0, 10), (71, 0), (71, 1001)):
            self.assertEqual(self.lib.bs_tx_spacing_ms(airtime, duty), 0)

    def test_crc_matches_zlib_known_and_binary_vectors(self):
        """Verify crc matches zlib known and binary vectors.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        for data in (b"", b"123456789", bytes(range(256)) * 17, b"\x00\xff" * 123):
            with self.subTest(size=len(data)):
                self.assertEqual(self.lib.bs_crc32(buffer(data), len(data)), zlib.crc32(data))
        self.assertEqual(self.lib.bs_crc32(buffer(b"123456789"), 9), 0xCBF43926)

    def test_data_and_end_equal_python_encoder_for_fragment_boundaries(self):
        """Verify data and end equal python encoder for fragment boundaries.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        for size in (1, 199, 200, 201, 400, 401, 233, 699):
            for part_size in (200, 233):
                with self.subTest(size=size, part_size=part_size):
                    data = bytes(i % 256 for i in range(size))
                    message = self.message(data, part_size)
                    golden = encode_message(Kind.IMAGE, 0x1234, 0x89ABCDEF, data, part_size)
                    self.assertEqual(message.count, len(golden))
                    for index, expected in enumerate(golden):
                        output = (C.c_uint8 * 255)()
                        length = self.lib.bs_encode_data(
                            C.byref(message), buffer(data), len(data), index, output, len(output)
                        )
                        actual = bytes(output[:length])
                        self.assertEqual(actual, expected)
                        decoded = decode_frame(actual)
                        self.assertEqual(decoded.device_id, 0x1234)
                        self.assertEqual(decoded.message_id, 0x89ABCDEF)
                    output = (C.c_uint8 * 255)()
                    length = self.lib.bs_encode_end(C.byref(message), output, len(output))
                    self.assertEqual(
                        bytes(output[:length]), encode_control(Kind.END, decode_frame(golden[0]))
                    )
                    self.assertEqual(bytes(output[:4]), b"BS\x01\x06")
                    self.assertEqual(bytes(output[4:10]), b"\x12\x34\x89\xab\xcd\xef")

    def test_invalid_empty_oversize_and_impossible_fragmentation_rejected(self):
        """Verify invalid empty oversize and impossible fragmentation rejected.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        message = Message()
        data = buffer(b"x")
        for kind, size, part_size in (
            (1, 0, 200),
            (4, 1, 200),
            (1, 1, 0),
            (1, 1, 234),
            (1, 1000001, 200),
            (1, 10001, 1),
        ):
            with self.subTest(kind=kind, size=size, part_size=part_size):
                self.assertLess(
                    self.lib.bs_message_init(C.byref(message), kind, 1, 1, data, size, part_size), 0
                )
        message = self.message(b"abc")
        output = (C.c_uint8 * 255)()
        self.assertEqual(
            self.lib.bs_encode_data(C.byref(message), buffer(b"abc"), 3, 1, output, 255),
            -errno.EINVAL,
        )
        self.assertEqual(
            self.lib.bs_encode_data(C.byref(message), buffer(b"abc"), 3, 0, output, 22),
            -errno.ENOBUFS,
        )
        self.assertEqual(self.lib.bs_encode_end(C.byref(message), output, 22), -errno.ENOBUFS)

    def test_python_ack_and_nack_decode_in_c(self):
        """Verify python ack and nack decode in c.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        data = b"x" * 8000
        original = decode_frame(encode_message(Kind.IMAGE, 0x1234, 0x89ABCDEF, data)[0])
        message = self.message(data)
        for kind, missing in (
            (Kind.ACK, ()),
            (Kind.NACK, (0, 2, 39)),
            (Kind.NACK, tuple(range(32))),
        ):
            packet = encode_control(kind, original, missing)
            feedback = Feedback()
            self.assertEqual(
                self.lib.bs_decode_feedback(
                    C.byref(message), buffer(packet), len(packet), C.byref(feedback)
                ),
                0,
            )
            self.assertEqual(feedback.kind, kind)
            self.assertEqual(tuple(feedback.missing[: feedback.missing_count]), missing)

    def test_malformed_and_each_unrelated_identity_field_rejected(self):
        """Verify malformed and each unrelated identity field rejected.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        data = b"a" * 600
        original = decode_frame(encode_message(Kind.IMAGE, 0x1234, 0x89ABCDEF, data)[0])
        message = self.message(data)
        ack = encode_control(Kind.ACK, original)
        feedback = Feedback()
        for offset in (0, 1, 2, 3, 10, 22):
            changed = bytearray(ack)
            changed[offset] ^= 0x80
            self.assertLess(
                self.lib.bs_decode_feedback(
                    C.byref(message), buffer(changed), len(changed), C.byref(feedback)
                ),
                0,
            )
        for offset in (4, 6, 12, 14, 18):
            changed = bytearray(ack)
            changed[offset + (1 if offset in (4, 12) else 3)] ^= 1
            self.assertLess(
                self.lib.bs_decode_feedback(
                    C.byref(message), buffer(changed), len(changed), C.byref(feedback)
                ),
                0,
            )
        nack = encode_control(Kind.NACK, original, (0, 2))
        for packet in (
            ack[:-1],
            ack + b"x",
            nack[:-1],
            nack[:23],
            nack[:23] + struct.pack("!HH", 2, 0),
            nack[:23] + struct.pack("!HH", 1, 1),
            nack[:23] + struct.pack("!H", 3),
        ):
            self.assertLess(
                self.lib.bs_decode_feedback(
                    C.byref(message), buffer(packet), len(packet), C.byref(feedback)
                ),
                0,
            )

    def test_complete_message_requires_ack_and_reassembles_in_python(self):
        """Verify complete message requires ack and reassembles in python.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        data = b"a\x00b" * 200
        status, result = self.send(data)
        self.assertEqual(status, 0)
        self.assertEqual(
            (result.fragment_count, result.data_attempts, result.end_attempts), (3, 3, 1)
        )
        self.assertEqual(self.link.store.ready()[0].body, data)
        self.assertEqual(self.link.receive_calls, 1)

    def test_nack_retransmits_only_requested_fragments(self):
        """Verify nack retransmits only requested fragments.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        self.link.drop_parts = {1}
        status, result = self.send()
        self.assertEqual(status, 0)
        self.assertEqual([p.index for p in self.link.sent if isinstance(p, Fragment)], [0, 1, 2, 1])
        self.assertEqual((result.data_attempts, result.end_attempts), (4, 2))

    def test_nack_pagination_recovers_every_fragment_lost(self):
        """Verify nack pagination recovers every fragment lost.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        self.link.drop_parts = set(range(40))
        status, result = self.send(b"x" * 8000)
        self.assertEqual(status, 0)
        self.assertEqual((result.data_attempts, result.end_attempts), (80, 3))
        self.assertEqual(len(self.link.store.ready()[0].body), 8000)

    def test_lost_end_or_feedback_retries_end_only(self):
        """Verify lost end or feedback retries end only.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        for drop_end, drop_feedback in ((1, 0), (0, 1)):
            with self.subTest(drop_end=drop_end):
                self.link.drop_end_count = drop_end
                self.link.drop_feedback_count = drop_feedback
                self.link.sent.clear()
                status, result = self.send()
                self.assertEqual(status, 0)
                self.assertEqual((result.data_attempts, result.end_attempts), (3, 2))
                self.assertTrue(all(p.message_id == 0x89ABCDEF for p in self.link.sent))

    def test_feedback_timeout_is_bounded_and_does_not_resend_data(self):
        """Verify feedback timeout is bounded and does not resend data.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        self.link.drop_end_count = 100
        self.options.max_end_attempts = 3
        status, result = self.send()
        self.assertEqual(status, -errno.ETIMEDOUT)
        self.assertEqual((result.data_attempts, result.end_attempts), (3, 3))
        self.assertEqual(self.link.time_ms, 60012)

    def test_bad_irrelevant_and_delayed_feedback_safely_ignored(self):
        """Verify bad irrelevant and delayed feedback safely ignored.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        unrelated = decode_frame(encode_message(Kind.IMAGE, 99, 42, b"other")[0])
        self.link.inject = [b"garbage", encode_control(Kind.ACK, unrelated)]
        status, result = self.send()
        self.assertEqual(status, 0)
        self.assertEqual(result.ignored_feedback, 2)

    def test_busy_bad_feedback_does_not_restart_receive_deadline(self):
        """Verify busy bad feedback does not restart receive deadline.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        self.link.inject = [b"bad"] * 200
        self.options.feedback_timeout_ms = 10
        self.options.max_end_attempts = 2
        status, result = self.send()
        self.assertEqual(status, -errno.ETIMEDOUT)
        self.assertEqual(result.ignored_feedback, 20)
        self.assertEqual(result.end_attempts, 2)

    def test_transmit_errors_recover_with_bounded_same_packet_retries(self):
        """Verify transmit errors recover with bounded same packet retries.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        self.link.tx_failures = 1
        status, result = self.send()
        self.assertEqual(status, 0)
        self.assertEqual(result.data_attempts, 4)
        self.assertEqual([p.index for p in self.link.sent if isinstance(p, Fragment)][:2], [0, 0])

    def test_persistent_radio_errors_abort_after_packet_limit(self):
        """Verify persistent radio errors abort after packet limit.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        self.link.tx_failures = 100
        status, result = self.send()
        self.assertEqual(status, -errno.EIO)
        self.assertEqual((result.data_attempts, result.end_attempts), (2, 0))
        self.link.tx_failures = 0
        self.link.receive_error = -errno.EIO
        status, result = self.send()
        self.assertEqual(status, -errno.EIO)
        self.assertEqual(result.end_attempts, 1)

    def test_zero_retry_limits_rejected_before_radio_use(self):
        """Verify zero retry limits rejected before radio use.

        Args:
            None.

        Returns:
            None: Assertions raise on failure; unittest records the result.
        """
        self.options.max_end_attempts = 0
        status, result = self.send()
        self.assertEqual(status, -errno.EINVAL)
        self.assertEqual(self.link.sent, [])


class PersistentIDTests(unittest.TestCase):
    """Exercise the real C reservation allocator across simulated reboot and NVS errors.

    Args:
        None.

    Returns:
        PersistentIDTests: unittest fixture using a fake flash interface, not real hardware.
    """

    @classmethod
    def setUpClass(cls):
        """Compile the allocator with host-only flash/NVS declarations.

        Args:
            None.

        Returns:
            None: The actual allocator is loaded with a controllable persistence fake.
        """
        cls.build = tempfile.TemporaryDirectory(prefix="bs-id-tests-")
        library = Path(cls.build.name) / "ids.so"
        subprocess.run(
            [
                os.getenv("CC", "cc"),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                "-shared",
                "-fPIC",
                "-I",
                str(ROOT / "include"),
                "-I",
                str(ROOT / "tests/nvs_fake"),
                str(ROOT / "tests/message_ids_fake.c"),
                "-o",
                str(library),
            ],
            check=True,
        )
        cls.lib = C.CDLL(str(library))
        cls.lib.bs_message_id_next.argtypes = [C.POINTER(C.c_uint32)]
        cls.lib.fake_ids_high_water.argtypes = [C.c_uint32]

    @classmethod
    def tearDownClass(cls):
        """Remove the temporary allocator library.

        Args:
            None.

        Returns:
            None: Temporary build files are released.
        """
        cls.build.cleanup()

    def setUp(self):
        """Begin each allocator scenario with empty fake persistence.

        Args:
            None.

        Returns:
            None: Static allocator and fake NVS state are reset for the test.
        """
        self.lib.fake_ids_erase()

    def test_ids_are_committed_in_blocks_and_skip_unused_ids_on_reboot(self):
        """Verify a reboot advances to the durable high-water mark without ID reuse.

        Args:
            None.

        Returns:
            None: Assertions cover first allocation, block rollover and restart.
        """
        self.assertEqual(self.lib.bs_message_ids_init(), 0)
        value = C.c_uint32()
        for expected in range(1, 34):
            self.assertEqual(self.lib.bs_message_id_next(C.byref(value)), 0)
            self.assertEqual(value.value, expected)
        self.assertEqual(self.lib.fake_ids_writes(), 2)
        self.lib.fake_ids_reboot()
        self.assertEqual(self.lib.bs_message_ids_init(), 0)
        self.assertEqual(self.lib.bs_message_id_next(C.byref(value)), 0)
        self.assertEqual(value.value, 65)

    def test_failed_reservation_never_issues_an_uncommitted_id(self):
        """Verify an interrupted/failed NVS write cannot expose an ID from that block.

        Args:
            None.

        Returns:
            None: A sentinel output remains untouched until persistence succeeds.
        """
        self.assertEqual(self.lib.bs_message_ids_init(), 0)
        self.lib.fake_ids_write_error(-errno.EIO)
        value = C.c_uint32(999)
        self.assertEqual(self.lib.bs_message_id_next(C.byref(value)), -errno.EIO)
        self.assertEqual(value.value, 999)
        self.lib.fake_ids_reboot()
        self.lib.fake_ids_write_error(0)
        self.assertEqual(self.lib.bs_message_ids_init(), 0)
        self.assertEqual(self.lib.bs_message_id_next(C.byref(value)), 0)
        self.assertEqual(value.value, 1)

    def test_corrupt_storage_and_exhausted_counter_fail_closed(self):
        """Verify read failures, invalid persisted zero and exhaustion do not reset IDs.

        Args:
            None.

        Returns:
            None: No sequence wrap or fallback-to-one occurs for detected errors.
        """
        self.lib.fake_ids_read_error(-errno.EIO)
        self.assertEqual(self.lib.bs_message_ids_init(), -errno.EIO)
        self.lib.fake_ids_read_error(0)
        self.lib.fake_ids_high_water(0)
        self.assertEqual(self.lib.bs_message_ids_init(), -errno.EIO)
        self.lib.fake_ids_high_water(0xFFFFFFFF)
        self.assertEqual(self.lib.bs_message_ids_init(), 0)
        value = C.c_uint32(999)
        self.assertEqual(self.lib.bs_message_id_next(C.byref(value)), -errno.EOVERFLOW)
        self.assertEqual(value.value, 999)


if __name__ == "__main__":
    unittest.main()
