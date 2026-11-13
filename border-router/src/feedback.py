"""One ACK or NACK per received END, never a response per DATA fragment.

All radio I/O happens within one event loop, because SX1262 is half-duplex.
"""

import logging
import math
import time
from dataclasses import dataclass

from .protocol import MAX_NACK_INDICES, Fragment, Kind, decode_frame, encode_control
from .store import MessageMeta, MessageStore

LOG = logging.getLogger(__name__)


def lora_airtime_seconds(size: int, *, sf: int, bw: int, cr: int, preamble: int = 12) -> float:
    """Calculate LoRa packet airtime with explicit headers, PHY CRC and automatic slow-symbol
    optimization.

    Args:
        size (int): Complete radio payload length in bytes, from 1 through 255.
        sf (int): LoRa spreading factor, from 5 through 12.
        bw (int): Supported radio bandwidth in Hz (125000, 250000 or 500000).
        cr (int): Coding-rate denominator, from 5 through 8.
        preamble (int): Number of programmed preamble symbols, normally 12.

    Returns:
        float: Estimated on-air duration in seconds, excluding amplifier ramp time.
    """
    if (
        not 1 <= size <= 255
        or not 5 <= sf <= 12
        or bw not in (125_000, 250_000, 500_000)
        or not 5 <= cr <= 8
        or preamble < 0
    ):
        raise ValueError("invalid modulation settings")
    tsym = (2**sf) / bw
    ldro = int(tsym >= 0.016)
    # SX1262 SF5/SF6 use different header and preamble overhead from SF7..SF12.
    # See Semtech sx126x_get_lora_time_on_air_numerator in sx126x_driver.
    payload_bits = 8 * size - 4 * sf + 16 + 20 + (8 if sf >= 7 else 0)
    payload_symbols = 8 + max(0, math.ceil(payload_bits / (4 * (sf - 2 * ldro)))) * cr
    preamble_overhead = 6.25 if sf <= 6 else 4.25
    return (preamble + preamble_overhead + payload_symbols) * tsym


@dataclass(frozen=True)
class Feedback:
    """Keep one encoded END response together with its message identity.

    Args:
        packet (bytes): Encoded ACK or NACK frame.
        kind (Kind): ACK or NACK.
        meta (MessageMeta): Persisted metadata used to construct the response.

    Returns:
        Feedback: Immutable pending transmit item.
    """

    packet: bytes
    kind: Kind
    meta: MessageMeta

    @property
    def key(self) -> tuple[int, int, int]:
        """Identify the message whose pending response is consumed after one transmit attempt.

        Args:
            None.

        Returns:
            tuple[int, int, int]: Device ID, original DATA kind and message ID.
        """
        return (self.meta.device_id, int(self.meta.kind), self.meta.message_id)


class FeedbackController:
    """Schedule at most one whole-message reply per END. Silence alone never creates feedback.

    Args:
        store (MessageStore): Durable fragment store and shared transmit-airtime ledger.
        quiet_seconds (float): Minimum receive silence before replying to END.
        sf (int): LoRa spreading factor used by the actual radio.
        bw (int): Actual radio bandwidth in Hz.
        cr (int): Actual coding-rate denominator (5 through 8).
        duty_cycle (float): Allowed TX fraction, normally 0.01; 0 bypasses all airtime limits.

    Returns:
        FeedbackController: Scheduler sharing the durable message and airtime store.
    """

    def __init__(
        self,
        store: MessageStore,
        *,
        quiet_seconds: float = 0.35,
        sf: int = 7,
        bw: int = 125_000,
        cr: int = 5,
        duty_cycle: float = 0.01,
    ) -> None:
        """Validate timing and modulation and create an empty END-response queue.

        Args:
            store (MessageStore): Durable fragment store and shared transmit-airtime ledger.
            quiet_seconds (float): Minimum receive silence before replying to END.
            sf (int): LoRa spreading factor used by the actual radio.
            bw (int): Actual radio bandwidth in Hz.
            cr (int): Actual coding-rate denominator (5 through 8).
            duty_cycle (float): Allowed TX fraction, normally 0.01; 0 bypasses all airtime
                limits.

        Returns:
            None: This operation returns no value.
        """
        if not math.isfinite(quiet_seconds) or quiet_seconds < 0:
            raise ValueError("quiet period must be nonnegative and finite")
        if not math.isfinite(duty_cycle) or not 0 <= duty_cycle <= 1:
            raise ValueError("duty cycle must be a fraction in 0..1; 0 disables the limiter")
        lora_airtime_seconds(1, sf=sf, bw=bw, cr=cr)
        self.store = store
        self.quiet_seconds = quiet_seconds
        self.sf, self.bw, self.cr = sf, bw, cr
        self.duty_cycle = duty_cycle
        self.last_rx_at = float("-inf")
        # Queue only on END, not DATA. Each key has at most one pending reply.
        self.pending: dict[tuple[int, int, int], None] = {}

    def on_frame(
        self,
        raw: bytes,
        *,
        rssi: float | None = None,
        snr: float | None = None,
        now: float | None = None,
    ) -> bool:
        """Persist DATA or queue a response to END. Incoming peer ACK/NACK frames are ignored.

        Args:
            raw (bytes): Complete binary radio frame.
            rssi (float | None): Received signal strength in dBm, when measured.
            snr (float | None): Signal-to-noise ratio in dB, when measured.
            now (float | None): Monotonic receive time in seconds; None reads the system clock.

        Returns:
            bool: True only when this DATA frame completes a new, CRC-valid message.
        """
        packet = decode_frame(raw)
        self.last_rx_at = time.monotonic() if now is None else now
        if isinstance(packet, Fragment):
            return self.store.accept(packet, rssi=rssi, snr=snr)
        if packet.kind == Kind.END:
            meta = self.store.register_end(packet)
            key = (meta.device_id, int(meta.kind), meta.message_id)
            self.pending[key] = None
            LOG.debug("END received for %s", key)
        else:
            LOG.debug("Ignoring peer %s: receiver only handles DATA and END", packet.kind.name)
        return False

    def poll(self, *, now: float | None = None) -> Feedback | None:
        """Prepare an ACK or a bounded missing-fragment NACK after the receive quiet period.

        Args:
            now (float | None): Monotonic time in seconds; None reads the system clock.

        Returns:
            Feedback | None: Next END response, or None when no reply is ready.
        """
        now = time.monotonic() if now is None else now
        if now - self.last_rx_at < self.quiet_seconds:
            return None
        for key in list(self.pending):
            meta = self.store.info(key[0], Kind(key[1]), key[2])
            if meta is None:
                del self.pending[key]
                continue
            if meta.state != "partial":
                return Feedback(encode_control(Kind.ACK, meta), Kind.ACK, meta)
            missing = self.store.missing(meta, limit=MAX_NACK_INDICES)
            if missing:
                return Feedback(encode_control(Kind.NACK, meta, missing), Kind.NACK, meta)
            # If all parts arrived but whole-message CRC failed, `accept` removed
            # the row. END will have re-created it, so all indices are missing.
            del self.pending[key]
        return None

    def reserve_transmit(self, item: Feedback) -> bool:
        """Reserve persisted airtime before TX, including failed attempts. A zero duty fraction
        bypasses the guard.

        Args:
            item (Feedback): Encoded response whose airtime will be charged.

        Returns:
            bool: True when transmission may proceed; False while the duty budget requires
                waiting.
        """
        # To disable the airtime restrictions COMPLETELY, use --duty-cycle 0
        # (LORA_DUTY=0), or construct this controller with duty_cycle=0.
        # This bypasses both rolling budget and off-time, including saved history.
        # Keep the default 0.01 for the default European 868.1 MHz configuration.
        if self.duty_cycle == 0:
            return True
        airtime = lora_airtime_seconds(len(item.packet), sf=self.sf, bw=self.bw, cr=self.cr)
        # Include the configured 800 us PA ramp and a small timing margin.
        return self.store.reserve_airtime(airtime + 0.005, fraction=self.duty_cycle)

    def complete_attempt(self, item: Feedback) -> None:
        """Consume one queued reply after a transmit attempt. A new END is required to retry.

        Args:
            item (Feedback): Response that was attempted, successfully or otherwise.

        Returns:
            None: This operation returns no value.
        """
        self.pending.pop(item.key, None)
