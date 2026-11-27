"""Exercise RX recovery and idle diagnostics without SPI or GPIO hardware."""

import unittest
from unittest.mock import MagicMock, patch

from src.radio import RadioConfig, SX1262Transceiver


class RadioRecoveryTests(unittest.TestCase):
    """Model the driver's refusal to restart a radio that is already receiving.

    Args:
        *args (object): Positional unittest framework arguments.
        **kwargs (object): Keyword unittest framework arguments.

    Returns:
        RadioRecoveryTests: Hardware-free receive-loop regression fixture.
    """

    def setUp(self) -> None:
        """Configure radio constants and a stateful RX/standby model.

        Args:
            None.

        Returns:
            None: Initialize driver mocks and the feedback controller.
        """
        self.radio = MagicMock()
        self.radio.get.return_value = b"abc"
        self.radio.available.return_value = 3
        self.radio.packetRssi.return_value = -90
        self.radio.snr.return_value = 7.25
        self.radio.getMode.return_value = 0x50
        self.radio.getIrqStatus.return_value = 0
        self.radio.readRegister.return_value = (0x07, 0x41)
        self.radio.getStatus.return_value = 0x24
        self.radio._readBytes.return_value = (0x24, 0x00, 0x00)
        for name, value in {
            "STANDBY_RC": 0,
            "IRQ_RX_DONE": 0x0002,
            "IRQ_HEADER_ERR": 0x0020,
            "IRQ_CRC_ERR": 0x0040,
            "IRQ_TIMEOUT": 0x0200,
            "STATUS_RX_DONE": 7,
            "STATUS_HEADER_ERR": 9,
            "STATUS_CRC_ERR": 8,
        }.items():
            setattr(self.radio, name, value)
        self.receiving = False
        self.radio.request.side_effect = self._request
        self.radio.setStandby.side_effect = self._standby
        self.radio.wait.return_value = True
        self.controller = MagicMock()
        self.controller.sf, self.controller.bw, self.controller.cr = 7, 125000, 5
        self.controller.poll.return_value = None
        driver = MagicMock()
        driver.SX126x.return_value = self.radio
        self.modules = {"gpiod": MagicMock(), "gpiod.line": MagicMock(), "LoRaRF": driver}

    def _request(self) -> bool:
        """Refuse a new RX request while the previous RX operation remains active.

        Args:
            None.

        Returns:
            bool: True when a new receive operation starts, otherwise False.
        """
        if self.receiving:
            return False
        self.receiving = True
        return True

    def _standby(self, mode: int) -> None:
        """End RX when the application explicitly returns the chip to standby.

        Args:
            mode (int): Requested standby mode.

        Returns:
            None: Clear the simulated active receive operation.
        """
        self.assertEqual(mode, self.radio.STANDBY_RC)
        self.receiving = False

    def test_receive_after_phy_error_does_not_stall_in_active_rx(self) -> None:
        """Deliver the next valid packet even when a PHY error leaves the chip in RX.

        Args:
            None.

        Returns:
            None: Assert recovery after both header and payload CRC errors.
        """
        for irq, status in (
            (self.radio.IRQ_HEADER_ERR, self.radio.STATUS_HEADER_ERR),
            (self.radio.IRQ_CRC_ERR, self.radio.STATUS_CRC_ERR),
        ):
            with self.subTest(irq=irq):
                self.receiving = False
                self.controller.on_frame.reset_mock()
                self.radio.getIrqStatus.side_effect = [irq, self.radio.IRQ_RX_DONE]
                self.radio.status.side_effect = [status, self.radio.STATUS_RX_DONE]
                with (
                    patch.dict("sys.modules", self.modules),
                    self.assertLogs("src.radio", level="INFO"),
                ):
                    SX1262Transceiver(RadioConfig()).run(
                        MagicMock(side_effect=[False, False, True]), self.controller
                    )
                self.controller.on_frame.assert_called_once_with(b"abc", rssi=-90, snr=7.25)

    def test_progress_irq_keeps_receiving_until_rx_done(self) -> None:
        """Ignore a header-valid IRQ while a packet is still arriving.

        Args:
            None.

        Returns:
            None: Assert only RX_DONE finalizes the packet and RX is not restarted early.
        """
        self.radio.getIrqStatus.side_effect = [0x0010, self.radio.IRQ_RX_DONE]
        self.radio.status.return_value = self.radio.STATUS_RX_DONE
        with (
            patch.dict("sys.modules", self.modules),
            patch("src.radio.time.sleep"),
            self.assertLogs("src.radio", level="INFO"),
        ):
            SX1262Transceiver(RadioConfig()).run(
                MagicMock(side_effect=[False, False, True]), self.controller
            )
        self.radio.wait.assert_called_once()
        self.radio.request.assert_called_once()
        self.controller.on_frame.assert_called_once()

    def test_idle_receiver_reports_status_at_info(self) -> None:
        """Show that IRQ polling continues when no transmitter is active.

        Args:
            None.

        Returns:
            None: Assert idle counters appear and no fake receive event is reported.
        """
        with (
            patch.dict("sys.modules", self.modules),
            patch("src.radio.time.sleep"),
            patch("src.radio.time.monotonic", side_effect=[0.0, 0.0, 15.0]),
            self.assertLogs("src.radio", level="INFO") as captured,
        ):
            SX1262Transceiver(RadioConfig()).run(
                MagicMock(side_effect=[False, False, True]), self.controller
            )
        logs = "\n".join(captured.output)
        self.assertIn("LoRa listening", logs)
        self.assertIn("rx_packets=0 header_errors=0 crc_errors=0", logs)
        self.assertIn("last_rx=never", logs)
        self.controller.on_frame.assert_not_called()

    def test_rejected_rx_request_fails_instead_of_silently_retrying_forever(self) -> None:
        """Report a radio that refuses RX even after being put in standby.

        Args:
            None.

        Returns:
            None: Assert an actionable failure and hardware cleanup.
        """
        self.radio.request.side_effect = None
        self.radio.request.return_value = False
        with (
            patch.dict("sys.modules", self.modules),
            patch("src.radio.time.sleep"),
            self.assertRaisesRegex(RuntimeError, "RX request rejected"),
        ):
            SX1262Transceiver(RadioConfig()).run(
                MagicMock(side_effect=[False, True]), self.controller
            )
        self.radio.end.assert_called_once()

    def test_invalid_irq_is_a_spi_failure_not_a_phy_header_error(self) -> None:
        """Reject the reserved IRQ bits seen in the Pi's 0x2d2d response.

        Args:
            None.

        Returns:
            None: Assert corrupt SPI data never reaches the packet decoder.
        """
        self.radio.getIrqStatus.return_value = 0x2D2D
        with (
            patch.dict("sys.modules", self.modules),
            self.assertRaisesRegex(RuntimeError, "Invalid SX1262 IRQ=0x2d2d: reserved bits"),
        ):
            SX1262Transceiver(RadioConfig()).run(lambda: False, self.controller)
        self.radio.wait.assert_not_called()
        self.controller.on_frame.assert_not_called()
        self.radio.end.assert_called_once()

    def test_sync_word_must_read_back_before_receiving(self) -> None:
        """Fail startup when SPI cannot read back the configured synchronization registers.

        Args:
            None.

        Returns:
            None: Assert the expected and observed values are reported before RX starts.
        """
        self.radio.readRegister.return_value = (0x2D, 0x2D)
        with (
            patch.dict("sys.modules", self.modules),
            self.assertRaisesRegex(RuntimeError, "register readback.*expected=0741.*actual=2d2d"),
        ):
            SX1262Transceiver(RadioConfig()).run(lambda: False, self.controller)
        self.radio.request.assert_not_called()
        self.radio.end.assert_called_once()

    def test_failed_diagnostics_do_not_hide_the_invalid_irq(self) -> None:
        """Preserve the original SPI diagnosis when subsequent status reads also fail.

        Args:
            None.

        Returns:
            None: Assert the invalid IRQ and secondary read failure are both reported.
        """
        self.radio.getIrqStatus.return_value = 0x2D2D
        self.radio.getStatus.side_effect = IndexError("empty SPI response")
        with (
            patch.dict("sys.modules", self.modules),
            self.assertRaisesRegex(RuntimeError, "IRQ=0x2d2d.*diagnostic_read_failed=IndexError"),
        ):
            SX1262Transceiver(RadioConfig()).run(lambda: False, self.controller)

    def test_standby_is_not_reported_as_listening(self) -> None:
        """Detect a rejected SetRx command even when request() returned True.

        Args:
            None.

        Returns:
            None: Assert standby produces a mode/device-error diagnosis instead of idle logs.
        """
        self.radio.getMode.return_value = 0x20
        self.radio.getStatus.return_value = 0x2A
        self.radio._readBytes.return_value = (0x2A, 0x01, 0x20)
        with (
            patch.dict("sys.modules", self.modules),
            self.assertRaisesRegex(RuntimeError, "not in RX.*mode=0x20.*device_errors=0x0120"),
        ):
            SX1262Transceiver(RadioConfig()).run(lambda: False, self.controller)
        self.controller.on_frame.assert_not_called()

    def test_rx_mode_is_rechecked_when_idle(self) -> None:
        """Detect a radio falling out of RX after its initial mode check passed.

        Args:
            None.

        Returns:
            None: Assert a later standby state cannot keep producing listening messages.
        """
        self.radio.getMode.side_effect = [0x50, 0x20]
        with (
            patch.dict("sys.modules", self.modules),
            patch("src.radio.time.sleep"),
            patch("src.radio.time.monotonic", side_effect=[0.0, 0.0, 15.0]),
            self.assertRaisesRegex(RuntimeError, "not in RX.*mode=0x20"),
        ):
            SX1262Transceiver(RadioConfig()).run(lambda: False, self.controller)

    def test_spi_clock_is_passed_to_the_driver(self) -> None:
        """Apply the requested SPI clock rather than the upstream 8 MHz default.

        Args:
            None.

        Returns:
            None: Assert that a slower diagnostic clock reaches LoRaSpi.
        """
        with patch.dict("sys.modules", self.modules):
            SX1262Transceiver(RadioConfig(spi_speed=500_000)).run(
                MagicMock(side_effect=[False, True]), self.controller
            )
        self.modules["LoRaRF"].LoRaSpi.assert_called_once_with(0, 0, 500_000)

    def test_full_size_rx_is_restored_after_sending_short_feedback(self) -> None:
        """Receive full-size DATA after the driver sets its packet length for a short ACK.

        Args:
            None.

        Returns:
            None: Assert feedback TX cannot leave RX limited to the ACK length.
        """
        limit = 0

        def packet_params(header: int, preamble: int, size: int, crc: bool, iq: bool) -> None:
            """Track the payload limit shared by SX1262 TX and RX.

            Args:
                header (int): Header mode.
                preamble (int): Preamble length.
                size (int): Payload limit programmed into the radio.
                crc (bool): PHY CRC setting.
                iq (bool): IQ inversion setting.

            Returns:
                None: Update the simulated hardware payload limit.
            """
            nonlocal limit
            limit = size

        def transmit() -> bool:
            """Model endPacket programming a short ACK length into the shared radio.

            Args:
                None.

            Returns:
                bool: True after configuring the simulated 23-byte feedback packet.
            """
            nonlocal limit
            limit = 23
            return True

        def irq_status() -> int:
            """Check that DATA reception uses the full radio payload limit.

            Args:
                None.

            Returns:
                int: Zero because no packet has arrived in this test.
            """
            self.assertEqual(limit, 255)
            return 0

        self.radio.setLoRaPacket.side_effect = packet_params
        self.radio.endPacket.side_effect = transmit
        self.radio.getIrqStatus.side_effect = irq_status
        self.radio.status.return_value = self.radio.STATUS_TX_DONE
        feedback = MagicMock(packet=b"x" * 23)
        self.controller.poll.side_effect = [feedback, None]
        with (
            patch.dict("sys.modules", self.modules),
            patch("src.radio.time.sleep"),
            self.assertLogs("src.radio", level="INFO"),
        ):
            SX1262Transceiver(RadioConfig()).run(
                MagicMock(side_effect=[False, False, True]), self.controller
            )
        self.controller.complete_attempt.assert_called_once_with(feedback)
