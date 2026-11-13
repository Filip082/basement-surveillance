"""Raspberry Pi 4B: SX1262 half-duplex RX/TX using current LoRaRF-Python API.

SPI0 CE0 is owned by /dev/spidev0.0. Hardware CE is used, so LoRaRF's
separate CS GPIO object is a no-op. Polling IRQ means DIO1 is optional.
"""

import logging
import time
from collections.abc import Callable
from contextlib import ExitStack
from dataclasses import dataclass
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from gpiod import LineRequest
    from gpiod.line import Value

from .feedback import FeedbackController, lora_airtime_seconds
from .protocol import MAX_RADIO_PAYLOAD, ProtocolError

LOG = logging.getLogger(__name__)


@dataclass(frozen=True)
class RadioConfig:
    """Configure Pi SPI0 CE0 wiring and Wio-SX1262 LoRa modulation. GPIO numbers are line offsets.

    Args:
        spi_bus (int): SPI controller number; this wiring supports bus 0.
        spi_cs (int): Hardware chip-select number; this wiring supports CE0.
        gpio_chip (int): Linux gpiochip index whose offsets match the wired GPIOs.
        reset_gpio (int): Active-low NRST line offset, normally BCM22.
        busy_gpio (int): BUSY input line offset, normally BCM23.
        rf_switch_gpio (int): RF_SW output offset; high selects RX and low selects TX.
        frequency (int): Carrier frequency in Hz; defaults to 868100000.
        spreading_factor (int): LoRa spreading factor, 5 through 12.
        bandwidth (int): LoRa channel bandwidth in Hz (125000, 250000 or 500000).
        coding_rate (int): Denominator of the LoRa coding rate 4/5 through 4/8.
        sync_word (int): Full SX1262 synchronization word, normally 0x0741.
        tx_power (int): Nominal PA preset in dBm: 14, 17, 20 or 22.
        poll_interval (float): Seconds between IRQ and feedback checks.

    Returns:
        RadioConfig: Immutable validated radio configuration.
    """

    spi_bus: int = 0
    spi_cs: int = 0
    gpio_chip: int = 0  # Raspberry Pi 4B BCM numbering on gpiochip0
    reset_gpio: int = 22
    busy_gpio: int = 23
    rf_switch_gpio: int = 24  # Wio SX1262 RF_SW: HIGH RX, LOW TX
    frequency: int = 868_100_000
    spreading_factor: int = 7
    bandwidth: int = 125_000
    coding_rate: int = 5
    sync_word: int = 0x0741  # Semtech SX1262 private sync word, 16-bit
    tx_power: int = 14
    poll_interval: float = 0.02

    def __post_init__(self) -> None:
        """Validate settings before the driver can silently clamp modulation.

        Args:
            None. Values come from this configuration instance.

        Returns:
            None: Raise ValueError if hardware or modulation settings are invalid.
        """
        pins = (self.reset_gpio, self.busy_gpio, self.rf_switch_gpio)
        if len(set(pins)) != len(pins) or any(p < 0 or p in (8, 9, 10, 11) for p in pins):
            raise ValueError("RESET, BUSY and RF_SW must be distinct GPIOs outside SPI0 pins 8..11")
        if self.spi_bus != 0 or self.spi_cs != 0 or self.gpio_chip < 0:
            raise ValueError("this wiring supports SPI0 CE0 and a nonnegative GPIO chip")
        if not 862_000_000 <= self.frequency <= 930_000_000:
            raise ValueError("Wio-SX1262 frequency must be in 862..930 MHz")
        if self.tx_power not in (14, 17, 20, 22):
            # The pinned driver selects optimized PA presets rather than exact steps.
            raise ValueError("LoRaRF SX1262 supports power presets 14, 17, 20 or 22 dBm")
        if not 0x100 <= self.sync_word <= 0xFFFF:
            raise ValueError("sync word must be a full 16-bit SX1262 value")
        if not 0 < self.poll_interval <= 1:
            raise ValueError("poll interval must be in (0, 1] seconds")
        lora_airtime_seconds(1, sf=self.spreading_factor, bw=self.bandwidth, cr=self.coding_rate)


class HeldOutputPin:
    """Adapt a GPIO output held for the radio lifetime to the LoRaRF pin interface.

    Args:
        request (gpiod.LineRequest): Active line request owned by the radio loop.
        offset (int): Requested GPIO line offset.

    Returns:
        HeldOutputPin: Output adapter that does not release ownership between writes.
    """

    def __init__(self, request: LineRequest, offset: int) -> None:
        """Bind an output adapter to an already requested GPIO line.

        Args:
            request (gpiod.LineRequest): Active line request, released by its owner.
            offset (int): GPIO line offset in that request.

        Returns:
            None: This operation returns no value.
        """
        self.request = request
        self.offset = offset

    def output(self, value: Value | int) -> None:
        """Set a held output using either a driver enum or an integer logic level.

        Args:
            value (gpiod.line.Value | int): ACTIVE/1 drives high; INACTIVE/0 drives low.

        Returns:
            None: This operation returns no value.
        """
        from gpiod.line import Value

        self.request.set_value(
            self.offset, Value.ACTIVE if value in (Value.ACTIVE, 1) else Value.INACTIVE
        )


class HardwareChipSelect:
    """Leave chip select under spidev control so GPIO8 has only one owner.

    Args:
        None.

    Returns:
        HardwareChipSelect: No-op adapter for the driver chip-select interface.
    """

    def output(self, _value: object) -> None:
        """Accept driver chip-select writes while hardware SPI controls CE0.

        Args:
            _value (object): Requested logic level; intentionally ignored.

        Returns:
            None: This operation returns no value.
        """
        pass


class SX1262Transceiver:
    """Run half-duplex LoRa reception and END-triggered feedback on the same radio.

    Args:
        config (RadioConfig): Validated Pi wiring and LoRa settings.

    Returns:
        SX1262Transceiver: Radio-loop owner; GPIOs are acquired when run starts.
    """

    def __init__(self, config: RadioConfig) -> None:
        """Store the configuration without acquiring SPI or GPIO hardware.

        Args:
            config (RadioConfig): Validated Pi wiring and LoRa settings.

        Returns:
            None: This operation returns no value.
        """
        self.config = config

    def run(self, stop_requested: Callable[[], bool], controller: FeedbackController) -> None:
        """Receive DATA/END and transmit scheduled ACK/NACK until shutdown. Release hardware on
        exit.

        Args:
            stop_requested (Callable[[], bool]): Return True when the radio loop must stop.
            controller (FeedbackController): Durable assembly and feedback scheduler using
                matching modulation.

        Returns:
            None: This operation returns no value.
        """
        try:
            import gpiod
            from gpiod.line import Direction, Value
            from LoRaRF import LoRaGpio, LoRaSpi, SX126x
        except ImportError as exc:
            raise RuntimeError("Install uv radio extra: uv sync --extra radio") from exc

        cfg = self.config
        if (controller.sf, controller.bw, controller.cr) != (
            cfg.spreading_factor,
            cfg.bandwidth,
            cfg.coding_rate,
        ):
            raise ValueError("airtime calculation must use the actual radio modulation")
        with ExitStack() as stack:
            request = gpiod.request_lines(
                f"/dev/gpiochip{cfg.gpio_chip}",
                consumer="bs-border-router",
                config={
                    cfg.reset_gpio: gpiod.LineSettings(
                        direction=Direction.OUTPUT, output_value=Value.ACTIVE
                    ),
                    cfg.rf_switch_gpio: gpiod.LineSettings(
                        direction=Direction.OUTPUT, output_value=Value.ACTIVE
                    ),
                },
            )
            stack.callback(request.release)
            reset = HeldOutputPin(request, cfg.reset_gpio)
            rf_sw = HeldOutputPin(request, cfg.rf_switch_gpio)
            radio = SX126x(
                LoRaSpi(cfg.spi_bus, cfg.spi_cs),
                HardwareChipSelect(),
                reset,
                LoRaGpio(cfg.gpio_chip, cfg.busy_gpio),
            )
            if not radio.begin():
                raise RuntimeError("SX1262 begin failed: check SPI, RESET, BUSY, 3.3V")
            stack.callback(radio.end)
            # Semtech DIO2 controls the module's internal RF switch together with RF_SW.
            radio.setDio2RfSwitch(True)
            radio.setDio3TcxoCtrl(radio.DIO3_OUTPUT_1_8, radio.TCXO_DELAY_5)
            radio.setFrequency(cfg.frequency)
            # Keep the driver's PA preset and TX register paired. A register value
            # of 22 with the 14 dBm PA preset is intentional (Semtech table 13-21).
            # Overwriting just SetTxParams would change the actual output power.
            radio.setTxPower(cfg.tx_power, radio.TX_POWER_SX1262)
            # Match the LDRO assumption in lora_airtime_seconds at slow symbol rates.
            ldro = (2**cfg.spreading_factor) / cfg.bandwidth >= 0.016
            radio.setLoRaModulation(cfg.spreading_factor, cfg.bandwidth, cfg.coding_rate, ldro)
            radio.setLoRaPacket(radio.HEADER_EXPLICIT, 12, MAX_RADIO_PAYLOAD, True, False)
            radio.setSyncWord(cfg.sync_word)
            rf_sw.output(1)
            LOG.info(
                "Pi 4B SX1262 RX/TX %dHz SF%d BW%d CR4/%d sync=%#06x",
                cfg.frequency,
                cfg.spreading_factor,
                cfg.bandwidth,
                cfg.coding_rate,
                cfg.sync_word,
            )

            rx_started = False
            while not stop_requested():
                if not rx_started:
                    rf_sw.output(1)
                    if radio.request():
                        rx_started = True
                    else:
                        time.sleep(cfg.poll_interval)
                        continue

                if radio.getIrqStatus() != 0:
                    # LoRaRF wait() finalizes RX metadata. Do not call until an IRQ
                    # is observed; otherwise it busy-loops inside upstream driver.
                    if radio.wait(0.25):
                        state = radio.status()
                        if state == radio.STATUS_RX_DONE:
                            raw = radio.get(radio.available())
                            rssi, snr = radio.packetRssi(), radio.snr()
                            LOG.info("LoRa RX bytes=%d rssi=%s dBm snr=%s dB", len(raw), rssi, snr)
                            LOG.debug("LoRa RX raw=%s", raw.hex())
                            try:
                                controller.on_frame(raw, rssi=rssi, snr=snr)
                            except ProtocolError as exc:
                                LOG.warning("Rejected received frame: %s", exc)
                        elif state in (radio.STATUS_CRC_ERR, radio.STATUS_HEADER_ERR):
                            LOG.warning("PHY frame CRC/header error")
                        else:
                            LOG.debug("RX status: %s", state)
                    rx_started = False
                    continue

                pending = controller.poll()
                if pending is not None and controller.reserve_transmit(pending):
                    # Half duplex: stop RX, select TX RF path, send then return to RX.
                    radio.setStandby(radio.STANDBY_RC)
                    rx_started = False
                    try:
                        rf_sw.output(0)
                        radio.beginPacket()
                        radio.put(pending.packet)
                        if not radio.endPacket():
                            raise RuntimeError("endPacket rejected TX")
                        duration = lora_airtime_seconds(
                            len(pending.packet),
                            sf=cfg.spreading_factor,
                            bw=cfg.bandwidth,
                            cr=cfg.coding_rate,
                        )
                        if (
                            not radio.wait(max(5.0, duration * 3))
                            or radio.status() != radio.STATUS_TX_DONE
                        ):
                            raise RuntimeError("SX1262 TX timeout / status not TX_DONE")
                        LOG.info(
                            "Sent %s device=%s kind=%s id=%s",
                            pending.kind.name,
                            pending.meta.device_id,
                            pending.meta.kind.name,
                            pending.meta.message_id,
                        )
                    except Exception as exc:
                        # Duty budget was already reserved. Sender will retry END
                        # if it has not received a valid ACK/NACK.
                        LOG.exception("ACK/NACK TX attempt failed: %s", exc)
                    finally:
                        # A timeout may leave the chip transmitting; stop it before RX.
                        radio.setStandby(radio.STANDBY_RC)
                        rf_sw.output(1)
                        controller.complete_attempt(pending)
                    continue
                time.sleep(cfg.poll_interval)
