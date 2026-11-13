"""Raw-LoRa BS application protocol v1 (big endian), NOT LoRaWAN.

DATA(kind=1..3): 22-byte header + up to 233 bytes payload.
END(kind=6):   22-byte header + 1-byte original data kind.
ACK(kind=4):   22-byte header + 1-byte original data kind.
NACK(kind=5):  22-byte header + 1-byte original data kind + 1..32 uint16 indices.

All frames repeat message metadata: device id, message id, fragment count,
full body size, and full-body CRC32. ACK/NACK are legal ONLY in reply to END.
"""

import math
import struct
import zlib
from dataclasses import dataclass
from enum import IntEnum
from typing import Protocol

MAGIC = b"BS"
VERSION = 1
HEADER = struct.Struct("!2sBBHIHHII")
MAX_RADIO_PAYLOAD = 255
MAX_PART_SIZE = MAX_RADIO_PAYLOAD - HEADER.size
DEFAULT_PART_SIZE = 200
MAX_NACK_INDICES = 32


class ProtocolError(ValueError):
    """Report malformed radio frames, conflicting fragments or invalid assembled payloads.

    Args:
        *args (object): Error details passed to ValueError.

    Returns:
        ProtocolError: Exception instance describing the rejected data.
    """


class Kind(IntEnum):
    """Identify DATA categories and whole-message END/ACK/NACK control frames.

    Args:
        value (int): Wire value: ENTRY=1, IMAGE=2, TELEMETRY=3, ACK=4, NACK=5, END=6.

    Returns:
        Kind: Enumeration member corresponding to the wire value.
    """

    ENTRY = 1
    IMAGE = 2
    TELEMETRY = 3
    ACK = 4
    NACK = 5
    END = 6


DATA_KINDS = (Kind.ENTRY, Kind.IMAGE, Kind.TELEMETRY)
CONTROL_KINDS = (Kind.ACK, Kind.NACK, Kind.END)


@dataclass(frozen=True, slots=True)
class Fragment:
    """Represent one immutable DATA fragment with repeated whole-message metadata.

    Args:
        device_id (int): Unsigned 16-bit identifier of the sending node.
        message_id (int): Unsigned 32-bit identifier unique within the retention period.
        kind (Kind): Application payload category (ENTRY, IMAGE or TELEMETRY).
        count (int): Total number of DATA fragments.
        total_size (int): Complete application body length in bytes.
        crc32 (int): CRC32/ISO-HDLC checksum of the complete application body.
        index (int): Zero-based position of this fragment.
        data (bytes): Nonempty slice of the application body.

    Returns:
        Fragment: Immutable decoded DATA packet.
    """

    kind: Kind
    device_id: int
    message_id: int
    index: int
    count: int
    total_size: int
    crc32: int
    data: bytes


@dataclass(frozen=True, slots=True)
class Control:
    """Represent END, ACK or NACK addressing an entire message rather than one fragment.

    Args:
        device_id (int): Unsigned 16-bit identifier of the sending node.
        message_id (int): Unsigned 32-bit identifier unique within the retention period.
        kind (Kind): END, ACK or NACK control operation.
        count (int): Total number of DATA fragments.
        total_size (int): Complete application body length in bytes.
        crc32 (int): CRC32/ISO-HDLC checksum of the complete application body.
        for_kind (Kind): Original application DATA category.
        missing (tuple[int, ...]): Sorted unique indices requested by NACK; otherwise empty.

    Returns:
        Control: Immutable decoded control packet.
    """

    kind: Kind
    for_kind: Kind
    device_id: int
    message_id: int
    count: int
    total_size: int
    crc32: int
    missing: tuple[int, ...] = ()


type Packet = Fragment | Control


class MessageMetadata(Protocol):
    """Describe metadata shared by stored messages and DATA fragments.

    Args:
        None. Implement this structural interface through its attributes.

    Returns:
        MessageMetadata: Type contract used by control-frame encoders.
    """

    kind: Kind
    device_id: int
    message_id: int
    count: int
    total_size: int
    crc32: int


def _validate_meta(count: int, total_size: int, index: int = 0) -> None:
    """Reject fragment metadata that cannot fit the protocol size and index bounds.

    Args:
        count (int): Total number of DATA fragments.
        total_size (int): Complete application body length in bytes.
        index (int): Zero-based fragment index to validate; defaults to zero.

    Returns:
        None: This operation returns no value.
    """
    if not (1 <= count <= 65535 and count <= total_size <= 0xFFFFFFFF):
        raise ProtocolError("invalid count/total size")
    if not 0 <= index < count:
        raise ProtocolError("fragment index out of range")
    if total_size > count * MAX_PART_SIZE:
        raise ProtocolError("declared size impossible for fragment count")


def decode_frame(frame: bytes) -> Packet:
    """Validate and decode one raw-LoRa version 1 frame. Full-body CRC is checked during assembly.

    Args:
        frame (bytes): Complete radio payload, including its 22-byte header.

    Returns:
        Fragment | Control: Parsed DATA or control packet; malformed data raises ProtocolError.
    """
    if not HEADER.size < len(frame) <= MAX_RADIO_PAYLOAD:
        raise ProtocolError("invalid frame length")
    magic, version, raw_kind, device, mid, index, count, size, crc = HEADER.unpack_from(frame)
    if magic != MAGIC or version != VERSION:
        raise ProtocolError("unexpected magic/protocol version")
    try:
        kind = Kind(raw_kind)
    except ValueError as exc:
        raise ProtocolError("unknown packet kind") from exc
    _validate_meta(count, size, index)
    body = frame[HEADER.size :]
    if kind in DATA_KINDS:
        return Fragment(kind, device, mid, index, count, size, crc, body)

    if index != 0 or not body:
        raise ProtocolError("invalid control frame")
    try:
        for_kind = Kind(body[0])
    except ValueError as exc:
        raise ProtocolError("invalid target kind") from exc
    if for_kind not in DATA_KINDS:
        raise ProtocolError("control must target data kind")
    if kind == Kind.NACK:
        if len(body) < 3 or (len(body) - 1) % 2 or len(body) > 1 + 2 * MAX_NACK_INDICES:
            raise ProtocolError("invalid NACK length")
        missing = tuple(index for (index,) in struct.iter_unpack("!H", body[1:]))
        if tuple(sorted(set(missing))) != missing or any(x >= count for x in missing):
            raise ProtocolError("invalid NACK indices")
    else:
        if len(body) != 1:
            raise ProtocolError("END/ACK must not contain missing indices")
        missing = ()
    return Control(kind, for_kind, device, mid, count, size, crc, missing)


def encode_message(
    kind: Kind, device_id: int, message_id: int, body: bytes, part_size: int = DEFAULT_PART_SIZE
) -> list[bytes]:
    """Split a nonempty application body into DATA frames sharing a whole-body CRC32.

    Args:
        kind (Kind): Application payload category (ENTRY, IMAGE or TELEMETRY).
        device_id (int): Unsigned 16-bit identifier of the sending node.
        message_id (int): Unsigned 32-bit identifier unique within the retention period.
        body (bytes): Complete JSON object or JPEG bytes to transmit.
        part_size (int): Maximum bytes per fragment, from 1 through 233.

    Returns:
        list[bytes]: Ordered wire frames; send a separate END after the final DATA frame.
    """
    if kind not in DATA_KINDS or not 0 <= device_id <= 65535 or not 0 <= message_id <= 0xFFFFFFFF:
        raise ValueError("invalid kind/device/message id")
    if not body or not 1 <= part_size <= MAX_PART_SIZE:
        raise ValueError("nonempty data and fragment size 1..233 required")
    count = math.ceil(len(body) / part_size)
    if count > 65535 or len(body) > 0xFFFFFFFF:
        raise ValueError("message too large")
    crc = zlib.crc32(body) & 0xFFFFFFFF
    return [
        HEADER.pack(MAGIC, VERSION, int(kind), device_id, message_id, i, count, len(body), crc)
        + body[i * part_size : (i + 1) * part_size]
        for i in range(count)
    ]


def encode_control(
    kind: Kind, original: MessageMetadata | Control, missing: tuple[int, ...] = ()
) -> bytes:
    """Encode END, ACK or NACK with metadata matching the original message.

    Args:
        kind (Kind): END, ACK or NACK operation to encode.
        original (MessageMetadata | Control): DATA, stored metadata or a prior control packet
            identifying the message.
        missing (tuple[int, ...]): One to 32 ascending unique missing indices for NACK;
            otherwise empty.

    Returns:
        bytes: Complete binary control frame ready for radio transmission.
    """
    if kind not in CONTROL_KINDS:
        raise ValueError("control kind must be END/ACK/NACK")
    orig_kind = original.for_kind if isinstance(original, Control) else original.kind
    if orig_kind not in DATA_KINDS:
        raise ValueError("invalid target kind")
    if not 0 <= original.device_id <= 65535 or not 0 <= original.message_id <= 0xFFFFFFFF:
        raise ValueError("invalid ids")
    _validate_meta(original.count, original.total_size)
    if kind == Kind.NACK:
        if (
            not 1 <= len(missing) <= MAX_NACK_INDICES
            or tuple(sorted(set(missing))) != missing
            or any(not 0 <= idx < original.count for idx in missing)
        ):
            raise ValueError("NACK needs 1..32 sorted unique missing indices")
    elif missing:
        raise ValueError("only NACK can contain missing indexes")
    header = HEADER.pack(
        MAGIC,
        VERSION,
        int(kind),
        original.device_id,
        original.message_id,
        0,
        original.count,
        original.total_size,
        original.crc32,
    )
    return header + bytes([int(orig_kind)]) + b"".join(struct.pack("!H", i) for i in missing)
