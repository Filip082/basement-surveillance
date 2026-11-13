"""SQLite-backed fragment assembly, missing fragment tracking, and HTTP outbox."""

import math
import sqlite3
import time
import zlib
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from threading import RLock

from .protocol import Control, Fragment, Kind, ProtocolError


@dataclass(frozen=True)
class ReadyMessage:
    """Represent a complete durable outbox payload awaiting HTTP delivery.

    Args:
        device_id (int): Unsigned 16-bit identifier of the sending node.
        message_id (int): Unsigned 32-bit identifier unique within the retention period.
        kind (Kind): Application payload category (ENTRY, IMAGE or TELEMETRY).
        body (bytes): Reassembled application body.
        received_at (str): UTC ISO 8601 assembly timestamp.
        rssi (float | None): RSSI of the completing fragment in dBm.
        snr (float | None): SNR of the completing fragment in dB.

    Returns:
        ReadyMessage: Immutable HTTP outbox item.
    """

    device_id: int
    message_id: int
    kind: Kind
    body: bytes
    received_at: str
    rssi: float | None
    snr: float | None

    @property
    def idempotency_key(self) -> str:
        """Build a stable backend deduplication key shared by every retry of this message.

        Args:
            None.

        Returns:
            str: Version 1 key containing device ID, DATA kind and message ID.
        """
        return f"bs-v1:{self.device_id}:{int(self.kind)}:{self.message_id}"


@dataclass(frozen=True)
class MessageMeta:
    """Describe stored assembly state without loading the complete payload.

    Args:
        device_id (int): Unsigned 16-bit identifier of the sending node.
        message_id (int): Unsigned 32-bit identifier unique within the retention period.
        kind (Kind): Application payload category (ENTRY, IMAGE or TELEMETRY).
        count (int): Total number of DATA fragments.
        total_size (int): Complete application body length in bytes.
        crc32 (int): CRC32/ISO-HDLC checksum of the complete application body.
        state (str): partial, ready or sent persistence state.
        last_progress (float): Unix time of the last new fragment or completed assembly.

    Returns:
        MessageMeta: Immutable metadata snapshot for feedback decisions.
    """

    device_id: int
    kind: Kind
    message_id: int
    count: int
    total_size: int
    crc32: int
    state: str
    last_progress: float


class MessageStore:
    """Persist fragments, CRC-checked messages and TX airtime in SQLite WAL transactions.

    Args:
        path (str | Path): SQLite file path; parent directories are created.
        max_message_bytes (int): Largest accepted reassembled body in bytes.

    Returns:
        MessageStore: Thread-safe store shared by radio and HTTP workers.
    """

    def __init__(self, path: str | Path, *, max_message_bytes: int = 1_000_000) -> None:
        """Open or initialize schema 1 with durable writes and reject incompatible existing
        databases.

        Args:
            path (str | Path): SQLite file path; parent directories are created.
            max_message_bytes (int): Largest accepted reassembled body in bytes.

        Returns:
            None: This operation returns no value.
        """
        if max_message_bytes <= 0:
            raise ValueError("maximum message size must be positive")
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(
            str(path), check_same_thread=False, timeout=30, isolation_level=None
        )
        self.lock = RLock()
        self.max_message_bytes = max_message_bytes
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=FULL")
        self.db.execute("PRAGMA foreign_keys=ON")
        version = self.db.execute("PRAGMA user_version").fetchone()[0]
        existing = self.db.execute(
            "SELECT name FROM sqlite_master WHERE name='messages'"
        ).fetchone()
        if existing and version != 1:
            self.db.close()
            raise RuntimeError("Incompatible SQLite schema; use a fresh router-v1.sqlite3")
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS messages (
                device_id INTEGER NOT NULL, kind INTEGER NOT NULL,
                message_id INTEGER NOT NULL, part_count INTEGER NOT NULL,
                total_size INTEGER NOT NULL, crc32 INTEGER NOT NULL,
                state TEXT NOT NULL CHECK (state IN ('partial','ready','sent')),
                body BLOB, received_at TEXT, rssi REAL, snr REAL,
                updated_at REAL NOT NULL,
                sent_at REAL, retry_at REAL NOT NULL DEFAULT 0,
                PRIMARY KEY (device_id, kind, message_id)
            );
            CREATE TABLE IF NOT EXISTS fragments (
                device_id INTEGER NOT NULL, kind INTEGER NOT NULL,
                message_id INTEGER NOT NULL, part_index INTEGER NOT NULL,
                data BLOB NOT NULL,
                PRIMARY KEY (device_id, kind, message_id, part_index),
                FOREIGN KEY (device_id, kind, message_id)
                    REFERENCES messages(device_id, kind, message_id) ON DELETE CASCADE
            );
            CREATE TABLE IF NOT EXISTS tx_airtime (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                sent_at REAL NOT NULL,
                duration REAL NOT NULL
            );
            PRAGMA user_version=1;
        """)

    def accept(
        self, part: Fragment, *, rssi: float | None = None, snr: float | None = None
    ) -> bool:
        """Commit a fragment and assemble all parts when present. Invalid whole-body CRC discards
        the partial message.

        Args:
            part (Fragment): Validated DATA fragment from decode_frame.
            rssi (float | None): Completing fragment RSSI in dBm, if available.
            snr (float | None): Completing fragment SNR in dB, if available.

        Returns:
            bool: True only when this call commits a newly complete message; duplicates return
                False.
        """
        if part.total_size > self.max_message_bytes or part.count > 10_000:
            raise ProtocolError("message exceeds configured limits")
        key = (part.device_id, int(part.kind), part.message_id)
        now = time.time()
        with self.lock:
            self.db.execute("BEGIN IMMEDIATE")
            try:
                row = self.db.execute(
                    "SELECT part_count,total_size,crc32,state FROM messages "
                    "WHERE device_id=? AND kind=? AND message_id=?",
                    key,
                ).fetchone()
                if row is None:
                    self.db.execute(
                        "INSERT INTO messages(device_id,kind,message_id,part_count,total_size,"
                        "crc32,state,updated_at) VALUES(?,?,?,?,?,?,'partial',?)",
                        (*key, part.count, part.total_size, part.crc32, now),
                    )
                elif row[:3] != (part.count, part.total_size, part.crc32):
                    raise ProtocolError("conflicting metadata for existing message")
                elif row[3] != "partial":
                    self.db.execute("COMMIT")
                    return False

                old = self.db.execute(
                    "SELECT data FROM fragments WHERE device_id=? AND kind=? AND "
                    "message_id=? AND part_index=?",
                    (*key, part.index),
                ).fetchone()
                if old is not None and old[0] != part.data:
                    raise ProtocolError("conflicting duplicate fragment")
                if old is None:
                    self.db.execute(
                        "INSERT INTO fragments VALUES(?,?,?,?,?)", (*key, part.index, part.data)
                    )
                    # Only new fragments extend the incomplete-message retention.
                    self.db.execute(
                        "UPDATE messages SET updated_at=? "
                        "WHERE device_id=? AND kind=? AND message_id=?",
                        (now, *key),
                    )
                available = self.db.execute(
                    "SELECT COUNT(*) FROM fragments WHERE device_id=? AND kind=? AND message_id=?",
                    key,
                ).fetchone()[0]
                if available < part.count:
                    self.db.execute("COMMIT")
                    return False

                chunks = self.db.execute(
                    "SELECT part_index,data FROM fragments WHERE device_id=? AND kind=? "
                    "AND message_id=? ORDER BY part_index",
                    key,
                ).fetchall()
                data = b"".join(chunk for _, chunk in chunks)
                if (
                    [idx for idx, _ in chunks] != list(range(part.count))
                    or len(data) != part.total_size
                    or (zlib.crc32(data) & 0xFFFFFFFF) != part.crc32
                ):
                    # The next END recreates metadata and requests all fragments again.
                    self.db.execute(
                        "DELETE FROM messages WHERE device_id=? AND kind=? AND message_id=?", key
                    )
                    self.db.execute("COMMIT")
                    raise ProtocolError("assembled message failed length/CRC32 validation")

                stamp = datetime.now(UTC).isoformat()
                self.db.execute(
                    "UPDATE messages SET state='ready',body=?,received_at=?,rssi=?,snr=?,"
                    "updated_at=? WHERE device_id=? AND kind=? AND message_id=?",
                    (data, stamp, rssi, snr, now, *key),
                )
                self.db.execute(
                    "DELETE FROM fragments WHERE device_id=? AND kind=? AND message_id=?", key
                )
                self.db.execute("COMMIT")
                return True
            except Exception:
                if self.db.in_transaction:
                    self.db.execute("ROLLBACK")
                raise

    def info(self, device_id: int, kind: Kind, message_id: int) -> MessageMeta | None:
        """Read one message metadata snapshot under the shared connection lock.

        Args:
            device_id (int): Unsigned 16-bit identifier of the sending node.
            kind (Kind): Application payload category (ENTRY, IMAGE or TELEMETRY).
            message_id (int): Unsigned 32-bit identifier unique within the retention period.

        Returns:
            MessageMeta | None: Stored metadata, or None when the message is absent.
        """
        with self.lock:
            row = self.db.execute(
                "SELECT device_id,kind,message_id,part_count,total_size,crc32,state,updated_at "
                "FROM messages WHERE device_id=? AND kind=? AND message_id=?",
                (device_id, int(kind), message_id),
            ).fetchone()
        if row is None:
            return None
        return MessageMeta(row[0], Kind(row[1]), row[2], row[3], row[4], row[5], row[6], row[7])

    def missing(self, meta: MessageMeta, limit: int = 32) -> tuple[int, ...]:
        """Find the first absent fragment indices for a bounded NACK response.

        Args:
            meta (MessageMeta): Snapshot identifying the message and expected fragment count.
            limit (int): Positive maximum number of missing indices to return.

        Returns:
            tuple[int, ...]: Ascending missing indices; empty for complete messages.
        """
        if limit <= 0:
            raise ValueError("missing-fragment limit must be positive")
        if meta.state != "partial":
            return ()
        with self.lock:
            indexes = {
                r[0]
                for r in self.db.execute(
                    "SELECT part_index FROM fragments "
                    "WHERE device_id=? AND kind=? AND message_id=?",
                    (meta.device_id, int(meta.kind), meta.message_id),
                )
            }
        result = []
        for i in range(meta.count):
            if i not in indexes:
                result.append(i)
                if len(result) == limit:
                    break
        return tuple(result)

    def ready(self, limit: int = 10) -> list[ReadyMessage]:
        """Fetch the oldest complete messages whose HTTP retry delay has elapsed.

        Args:
            limit (int): Maximum number of outbox messages to load.

        Returns:
            list[ReadyMessage]: Ordered outbox batch containing complete application bodies.
        """
        if limit <= 0:
            raise ValueError("outbox batch size must be positive")
        with self.lock:
            rows = self.db.execute(
                "SELECT device_id,message_id,kind,body,received_at,rssi,snr "
                "FROM messages WHERE state='ready' AND retry_at<=? ORDER BY updated_at LIMIT ?",
                (time.time(), limit),
            ).fetchall()
        return [
            ReadyMessage(
                device_id=d, message_id=m, kind=Kind(k), body=bytes(b), received_at=t, rssi=r, snr=s
            )
            for d, m, k, b, t, r, s in rows
        ]

    def mark_sent(self, item: ReadyMessage) -> None:
        """Record successful HTTP delivery and release payload storage while retaining
        deduplication metadata.

        Args:
            item (ReadyMessage): Outbox item whose backend request succeeded.

        Returns:
            None: This operation returns no value.
        """
        with self.lock:
            self.db.execute(
                "UPDATE messages SET state='sent',body=NULL,sent_at=? WHERE "
                "device_id=? AND kind=? AND message_id=? AND state='ready'",
                (time.time(), item.device_id, int(item.kind), item.message_id),
            )

    def retry_later(self, item: ReadyMessage, *, delay_seconds: float = 5.0) -> None:
        """Defer a failed delivery so an oldest failing batch cannot starve newer messages.

        Args:
            item (ReadyMessage): Outbox item whose HTTP request or JSON validation failed.
            delay_seconds (float): Positive finite delay before the next delivery attempt.

        Returns:
            None: The body remains durable and the retry deadline is saved across restarts.
        """
        if not math.isfinite(delay_seconds) or delay_seconds <= 0:
            raise ValueError("retry delay must be positive and finite")
        with self.lock:
            self.db.execute(
                "UPDATE messages SET retry_at=? WHERE device_id=? AND kind=? "
                "AND message_id=? AND state='ready'",
                (time.time() + delay_seconds, item.device_id, int(item.kind), item.message_id),
            )

    def cleanup(
        self, *, incomplete_ttl_seconds: float = 172_800, sent_ttl_seconds: float = 604_800
    ) -> None:
        """Expire stalled assemblies and old delivery records. Undelivered complete bodies remain
        stored.

        Args:
            incomplete_ttl_seconds (float): Retention since last assembly progress, normally two
                days.
            sent_ttl_seconds (float): Deduplication retention after HTTP success, normally seven
                days.

        Returns:
            None: This operation returns no value.
        """
        with self.lock:
            self.db.execute(
                "DELETE FROM messages WHERE (state='partial' AND updated_at < ?) "
                "OR (state='sent' AND sent_at < ?)",
                (time.time() - incomplete_ttl_seconds, time.time() - sent_ttl_seconds),
            )

    def close(self) -> None:
        """Close the SQLite connection after both radio and HTTP workers have stopped.

        Args:
            None.

        Returns:
            None: This operation returns no value.
        """
        with self.lock:
            self.db.close()

    def register_end(self, control: Control) -> MessageMeta:
        """Store END metadata even when all DATA was lost, or reject metadata conflicting with an
        existing message.

        Args:
            control (Control): Validated END packet targeting an application DATA kind.

        Returns:
            MessageMeta: Matching persisted metadata used to choose ACK or NACK.
        """
        if not isinstance(control, Control) or control.kind != Kind.END:
            raise ProtocolError("expected END")
        if control.total_size > self.max_message_bytes or control.count > 10_000:
            raise ProtocolError("message exceeds configured limits")
        key = (control.device_id, int(control.for_kind), control.message_id)
        with self.lock:
            self.db.execute(
                "INSERT OR IGNORE INTO messages(device_id,kind,message_id,part_count,total_size,"
                "crc32,state,updated_at) VALUES(?,?,?,?,?,?,'partial',?)",
                (*key, control.count, control.total_size, control.crc32, time.time()),
            )
        meta = self.info(control.device_id, control.for_kind, control.message_id)
        if meta is None or (meta.count, meta.total_size, meta.crc32) != (
            control.count,
            control.total_size,
            control.crc32,
        ):
            raise ProtocolError("END does not match original message metadata")
        return meta

    def reserve_airtime(
        self, duration: float, *, fraction: float = 0.01, window: float = 3600.0
    ) -> bool:
        """Atomically reserve a rolling TX budget and post-TX off-time. Charge before transmission,
        even if TX fails.

        Args:
            duration (float): Positive planned transmit duration in seconds, including timing
                margin.
            fraction (float): Allowed TX fraction in (0, 1], normally 0.01.
            window (float): Positive rolling accounting interval in seconds, normally one hour.

        Returns:
            bool: True if the reservation was committed; False if airtime or off-time blocks TX.
        """
        if (
            not math.isfinite(duration)
            or duration <= 0
            or not math.isfinite(fraction)
            or not 0 < fraction <= 1
            or not math.isfinite(window)
            or window <= 0
        ):
            raise ValueError("invalid airtime/budget")
        now = time.time()
        with self.lock:
            self.db.execute("BEGIN IMMEDIATE")
            try:
                self.db.execute("DELETE FROM tx_airtime WHERE sent_at < ?", (now - window,))
                used = self.db.execute(
                    "SELECT COALESCE(SUM(duration),0) FROM tx_airtime"
                ).fetchone()[0]
                last = self.db.execute(
                    "SELECT sent_at,duration FROM tx_airtime ORDER BY sent_at DESC LIMIT 1"
                ).fetchone()
                allowed = used + duration <= fraction * window + 1e-9 and (
                    last is None or now >= last[0] + last[1] / fraction
                )
                if allowed:
                    self.db.execute(
                        "INSERT INTO tx_airtime(sent_at,duration) VALUES(?,?)", (now, duration)
                    )
                self.db.execute("COMMIT")
                return allowed
            except Exception:
                if self.db.in_transaction:
                    self.db.execute("ROLLBACK")
                raise
