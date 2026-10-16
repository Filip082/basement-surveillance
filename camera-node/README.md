# camera-node

Firmware for the DFRobot DFR1154 (ESP32-S3 AI CAM: ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM, OV3660), ESP-IDF v6.1.

## Open the dev container

1. Install Docker and VS Code with the **Dev Containers** extension
2. Open the `camera-node/` folder (not the repo root) in VS Code
3. Run **Dev Containers: Reopen in Container** (first build takes several minutes)

## Build (inside the container)

```bash
idf.py build
```

Output lands in `build/` (shared with the host).

## Flash

### Linux: from inside the container

```bash
idf.py -p /dev/ttyACM0 flash monitor   # /dev/ttyUSB0 if the board uses a USB-UART chip
```

If you get `Permission denied` on the port, add your host user to the `dialout`
group (`sudo usermod -aG dialout $USER`, then log out and back in).

### Windows: from the host

One-time: install Python, then `pip install esptool`.

```powershell
cd camera-node\build
python -m esptool --chip esp32s3 -p COM5 -b 460800 --before default-reset --after hard-reset write-flash "@flash_args"
python -m serial.tools.miniterm COM5 115200   # serial monitor, Ctrl+] to quit
```

Find the COM port in Device Manager → Ports (COM & LPT).

No-install alternative: <https://espressif.github.io/esptool-js/> (Chrome/Edge).
Pick the files and addresses listed in `build/flash_args`.

## Configuration

- `sdkconfig.defaults` is committed; `sdkconfig` is generated and ignored.
- After changing options with `idf.py menuconfig`, run `idf.py save-defconfig`
  and commit the updated `sdkconfig.defaults`.
