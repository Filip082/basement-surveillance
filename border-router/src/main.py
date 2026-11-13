"""Run with `uv run --extra radio bs-border-router` on Raspberry Pi 4B."""

import argparse
import logging
import os
import signal
import sys
import time
from threading import Event, Thread

from . import __version__
from .feedback import FeedbackController
from .http_client import HttpForwarder
from .protocol import ProtocolError
from .radio import RadioConfig, SX1262Transceiver
from .store import MessageStore

LOG = logging.getLogger(__name__)


def run(argv: list[str] | None = None) -> int:
    """Start the radio or stdin simulator and persistent HTTP worker, then shut down both cleanly.

    Args:
        argv (list[str] | None): CLI arguments without the program name; None uses sys.argv.

    Returns:
        int: Process exit code, zero on normal shutdown and one on radio-loop failure.
    """
    ap = argparse.ArgumentParser(description="SX1262 -> reliable messages -> HTTP backend")
    ap.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    ap.add_argument("--simulate-hex", action="store_true", help="accept hex radio frames on stdin")
    ap.add_argument("--db", default=os.getenv("BORDER_DB", "./router-v1.sqlite3"))
    ap.add_argument("--backend-url", default=os.getenv("BACKEND_URL", "http://127.0.0.1:8000"))
    ap.add_argument("--gpio-chip", type=int, default=os.getenv("GPIO_CHIP", "0"))
    ap.add_argument("--reset", type=int, default=os.getenv("RESET_GPIO", "22"))
    ap.add_argument("--busy", type=int, default=os.getenv("BUSY_GPIO", "23"))
    ap.add_argument("--rf-switch", type=int, default=os.getenv("RF_SWITCH_GPIO", "24"))
    ap.add_argument("--freq", type=int, default=os.getenv("LORA_FREQ", "868100000"))
    ap.add_argument("--sf", type=int, default=os.getenv("LORA_SF", "7"))
    ap.add_argument("--bw", type=int, default=os.getenv("LORA_BW", "125000"))
    ap.add_argument("--cr", type=int, default=os.getenv("LORA_CR", "5"))
    ap.add_argument(
        "--sync-word",
        type=lambda v: int(v, 0),
        default=os.getenv("LORA_SYNC_WORD", "0x0741"),
    )
    ap.add_argument("--tx-power", type=int, default=os.getenv("LORA_TX_POWER", "14"))
    ap.add_argument(
        "--duty-cycle",
        type=float,
        default=os.getenv("LORA_DUTY", "0.01"),
        help="TX airtime fraction (default: 0.01); 0 completely disables the limiter",
    )
    ap.add_argument("--quiet-seconds", type=float, default=0.35)
    ap.add_argument(
        "--log-level",
        type=str.upper,
        choices=("DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"),
        default="INFO",
    )
    opts = ap.parse_args(argv)
    logging.basicConfig(
        level=getattr(logging, opts.log_level.upper()),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    stop = Event()
    for signum in (signal.SIGTERM, signal.SIGINT):
        signal.signal(signum, lambda _signal, _frame: stop.set())

    store = MessageStore(opts.db)
    try:
        controller = FeedbackController(
            store,
            quiet_seconds=opts.quiet_seconds,
            sf=opts.sf,
            bw=opts.bw,
            cr=opts.cr,
            duty_cycle=opts.duty_cycle,
        )
        forwarder = HttpForwarder(opts.backend_url)
        cfg = RadioConfig(
            gpio_chip=opts.gpio_chip,
            reset_gpio=opts.reset,
            busy_gpio=opts.busy,
            rf_switch_gpio=opts.rf_switch,
            frequency=opts.freq,
            spreading_factor=opts.sf,
            bandwidth=opts.bw,
            coding_rate=opts.cr,
            sync_word=opts.sync_word,
            tx_power=opts.tx_power,
        )
    except ValueError as exc:
        store.close()
        ap.error(str(exc))

    def http_worker() -> None:
        """Retry persisted outbox deliveries and periodically expire partial and sent records until
        shutdown.

        Args:
            None.

        Returns:
            None: This operation returns no value.
        """
        cycles = 0
        while not stop.is_set():
            for msg in store.ready():
                if stop.is_set():
                    return
                try:
                    forwarder.deliver(msg)
                except ProtocolError as exc:
                    store.retry_later(msg)
                    LOG.error("Cannot forward invalid JSON %s: %s", msg.idempotency_key, exc)
                except Exception as exc:
                    store.retry_later(msg)
                    LOG.warning("HTTP retry %s: %s", msg.idempotency_key, exc)
                else:
                    store.mark_sent(msg)
                    LOG.info("HTTP delivered %s", msg.idempotency_key)
            cycles += 1
            if cycles % 600 == 0:
                store.cleanup()
            stop.wait(1.0)

    worker = Thread(target=http_worker, name="http-forwarder", daemon=True)
    worker.start()
    try:
        if opts.simulate_hex:
            LOG.info("Enter one hex-encoded radio frame per line. Feedback is printed in hex.")
            for line in sys.stdin:
                if stop.is_set():
                    break
                line = line.strip()
                if not line:
                    continue
                try:
                    controller.on_frame(bytes.fromhex(line), now=0)
                    feedback = controller.poll(now=opts.quiet_seconds)
                    if feedback:
                        print(f"{feedback.kind.name}: {feedback.packet.hex()}", flush=True)
                        controller.complete_attempt(feedback)
                except ValueError as exc:
                    LOG.warning("Invalid incoming frame: %s", exc)
            deadline = time.monotonic() + 5.0
            while store.ready() and time.monotonic() < deadline and not stop.is_set():
                time.sleep(0.1)
        else:
            SX1262Transceiver(cfg).run(stop.is_set, controller)
    except Exception:
        LOG.exception("Radio loop failed")
        return 1
    finally:
        stop.set()
        # The worker may still own the shared connection during an HTTP retry.
        worker.join()
        store.close()
    return 0


if __name__ == "__main__":
    sys.exit(run())
