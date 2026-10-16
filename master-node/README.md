# master-node

Firmware for the Seeed XIAO nRF52840 + Wio-SX1262 LoRa module, Zephyr v4.4.2.

| File                      | What                                                    |
|---------------------------|---------------------------------------------------------|
| `west.yml`                | Pins the Zephyr version and the modules we fetch        |
| `prj.conf`                | Kconfig options (LoRa enabled)                          |
| `boards/xiao_ble.overlay` | Wio-SX1262 wiring (SPI + control pins)                  |
| `src/main.c`              | Application code                                        |
| `CMakeLists.txt`          | Build setup (enables the USB console snippet)           |

## Open the dev container

1. Install Docker and VS Code with the **Dev Containers** extension
2. Open the `master-node/` folder (not the repo root) in VS Code
3. Run **Dev Containers: Reopen in Container**

The first start downloads Zephyr and its modules into a Docker volume (a few minutes).
Later starts reuse it.

## Build (inside the container)

```bash
west build -b xiao_ble          # incremental
west build -b xiao_ble -p       # pristine (clean) build
```

Output: `build/zephyr/zephyr.uf2` (shared with the host).

## Flash (any OS, no tools needed)

1. Build inside the container (see above). The file appears on the host at
   `master-node/build/zephyr/zephyr.uf2`
2. Plug the XIAO into your computer over USB-C
3. Double-tap the reset button. A new USB drive appears (the board's bootloader)
4. Copy `zephyr.uf2` from step 1 onto that drive (File Explorer on Windows, file manager or `cp` on Linux)
5. The drive disappears and the board reboots into the new firmware

## Console output (USB)

`printf` and logs go to a USB serial port on the same cable. Open it on the host:

- **Windows:** find the COM port in Device Manager → Ports (COM & LPT), then open it with
  PuTTY (Connection type: Serial, speed 115200) or `python -m serial.tools.miniterm COM6 115200`
- **Linux:** `screen /dev/ttyACM0 115200` (quit: Ctrl+A then K).
  `Permission denied` → `sudo usermod -aG dialout $USER`, then log out and back in

Messages printed before the port is opened are lost, e.g. the hello message right after boot.

## Notes

- USB console comes from Zephyr's `cdc-acm-console` snippet, set in `CMakeLists.txt`.
- Upgrading Zephyr: change `revision` in `west.yml` **and** `ZEPHYR_VERSION` /
  `ZEPHYR_SDK_VERSION` in `.devcontainer/Dockerfile`, then rebuild the container and
  run `west update` in `/workdir`.
