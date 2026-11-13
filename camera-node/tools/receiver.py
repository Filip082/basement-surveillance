#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = ["bleak>=0.22"]
# ///
"""
Receiver for the camera node (run it on the computer that should get the pictures).

It scans for the node, connects as soon as it appears, (pairs the first time), subscribes
to the Image characteristic, saves every JPEG into ./captures and confirms it with an ACK
so the node can go back to sleep immediately. Then it goes back to scanning, forever.

    uv run tools/receiver.py              # installs bleak automatically
    uv run tools/receiver.py --once       # stop after the first picture
"""
import argparse
import asyncio
import datetime
import pathlib
import struct
import zlib

from bleak import BleakClient, BleakScanner

SVC = "dfa10001-5b3c-4f6d-9e21-0a4c7b8d1e00"
CTRL = "dfa10002-5b3c-4f6d-9e21-0a4c7b8d1e00"   # write: 0x01 = ACK, 0x02 = resend
IMG = "dfa10003-5b3c-4f6d-9e21-0a4c7b8d1e00"    # notify: START / DATA / END


class Assembler:
    """Rebuilds one JPEG from the node's notification packets."""

    def __init__(self):
        self.complete = asyncio.Event()
        self.ok = False
        self.jpeg = b""
        self.info = ""
        self._reset()

    def _reset(self):
        self.buf = None
        self.expected = 0
        self.size = (0, 0)
        self.seq = 0

    def on_notify(self, _char, data: bytearray):
        kind = data[0]
        if kind == 1:                                   # START: len u32, w u16, h u16
            n, w, h = struct.unpack_from("<IHH", data, 1)
            self.buf, self.expected, self.size, self.seq = bytearray(), n, (w, h), 0
            self.t0 = asyncio.get_running_loop().time()
        elif kind == 2 and self.buf is not None:        # DATA: seq u16 + bytes
            (seq,) = struct.unpack_from("<H", data, 1)
            if seq != self.seq:
                print(f"  packet loss: expected {self.seq}, got {seq}")
                self._reset()
                return
            self.buf += data[3:]
            self.seq += 1
        elif kind == 3 and self.buf is not None:        # END: crc32 u32
            (crc,) = struct.unpack_from("<I", data, 1)
            dt = asyncio.get_running_loop().time() - self.t0
            self.ok = len(self.buf) == self.expected and zlib.crc32(self.buf) == crc
            self.jpeg = bytes(self.buf)
            self.info = (f"{self.size[0]}x{self.size[1]}, {len(self.buf)} bytes in {dt:.2f} s "
                         f"({len(self.buf) / dt / 1000:.1f} kB/s), CRC {'OK' if self.ok else 'MISMATCH'}")
            self._reset()
            self.complete.set()


async def session(device, args, outdir: pathlib.Path) -> bool:
    """One connection. Returns True if a picture was saved."""
    asm = Assembler()
    gone = asyncio.Event()
    saved = False
    async with BleakClient(device, disconnected_callback=lambda _c: gone.set(),
                           timeout=args.connect_timeout) as client:
        print(f"connected to {device.name or device.address}")
        if not args.no_pair:
            try:
                await client.pair()             # first time: bonds; afterwards: no-op
            except Exception as e:              # not supported on every OS
                print(f"  pair() skipped: {e}")
        await client.start_notify(IMG, asm.on_notify)
        print("  subscribed, waiting for the picture ...")

        for _attempt in range(2):
            waiters = [asyncio.ensure_future(asm.complete.wait()),
                       asyncio.ensure_future(gone.wait())]
            await asyncio.wait(waiters, timeout=args.session_timeout,
                               return_when=asyncio.FIRST_COMPLETED)
            for w in waiters:
                w.cancel()
            if not asm.complete.is_set():
                break
            asm.complete.clear()
            print(f"  {asm.info}")
            if asm.ok:
                name = outdir / f"capture_{datetime.datetime.now():%Y%m%d_%H%M%S}.jpg"
                name.write_bytes(asm.jpeg)
                print(f"  saved {name}")
                saved = True
                await client.write_gatt_char(CTRL, b"\x01", response=False)   # ACK
                break
            await client.write_gatt_char(CTRL, b"\x02", response=False)       # NAK -> resend
    return saved


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", default="DFR1154", help="device name prefix to look for")
    ap.add_argument("--out", default="captures", help="folder for the JPEG files")
    ap.add_argument("--once", action="store_true", help="exit after the first picture")
    ap.add_argument("--no-pair", action="store_true", help="do not call pair() (use OS pairing instead)")
    ap.add_argument("--connect-timeout", type=float, default=15.0)
    ap.add_argument("--session-timeout", type=float, default=20.0)
    args = ap.parse_args()

    outdir = pathlib.Path(args.out)
    outdir.mkdir(exist_ok=True)

    def match(dev, adv):
        name = adv.local_name or dev.name or ""
        return name.startswith(args.name) or SVC in [u.lower() for u in adv.service_uuids]

    print(f"waiting for \"{args.name}*\" - trigger the node's wake-up input ... (Ctrl+C to quit)")
    while True:
        device = await BleakScanner.find_device_by_filter(match, timeout=30.0)
        if device is None:
            continue
        try:
            if await session(device, args, outdir) and args.once:
                return
        except Exception as e:
            print(f"  session failed: {e}")
        await asyncio.sleep(0.3)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
