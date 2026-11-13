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

## Build with plain Docker (no dev container)

The toolchain lives in the `espressif/idf:v6.1` image, the same one the dev container uses.
From the project folder:

```bash
scripts/docker-build.sh            # Linux / macOS / WSL  -> idf.py build
scripts/docker-build.sh menuconfig
```

```powershell
.\scripts\docker-build.ps1          # Windows PowerShell (Docker Desktop)
.\scripts\docker-build.ps1 menuconfig
```

Output lands in `build/`; flash from the host as described under **Flash** above.
The first build downloads the `esp32-camera` component, so it needs internet access.
Set `IDF_IMAGE` to use a different image tag.

## Configuration

- `sdkconfig.defaults` is committed; `sdkconfig` is generated and ignored.
- After changing options with `idf.py menuconfig`, run `idf.py save-defconfig`
  and commit the updated `sdkconfig.defaults`.

## Camera + BLE demo

What the firmware does:

1. Initialises the OV3660 (JPEG, QVGA, frame buffers in PSRAM) and prints a self-test
   on the serial monitor: 3 frames, size, JPEG start/end markers, timing, `PASS`/`FAIL`.
2. Starts a BLE (NimBLE) peripheral named `DFR1154-CAM` that lets you trigger captures
   and receive the JPEG over notifications.

Source layout (`main/`):

| File | Purpose |
|---|---|
| `board_dfr1154.h` | GPIO map (camera, LED GPIO3, IR GPIO47) |
| `camera_service.c/.h` | camera init, capture, size/quality, self-test |
| `ble_service.c/.h` | GATT server, command handling, JPEG streaming |
| `camera_node.c` | `app_main` |
| `Kconfig.projbuild` | `idf.py menuconfig` -> *Camera node demo* (name, XCLK, quality, flip) |

### Trying it

1. `idf.py build`, flash, open the monitor. Expect `self-test: ... PASS` and `advertising as "DFR1154-CAM"`.
2. **PC:** open `tools/ble_viewer.html` in Chrome/Edge, click *Connect*, then *Capture*.
   **Phone:** nRF Connect -> connect -> enable notifications on the Image characteristic ->
   write `01` to Control.

### GATT protocol

Service `dfa10001-5b3c-4f6d-9e21-0a4c7b8d1e00`

| Char | UUID | Props | Use |
|---|---|---|---|
| Control | `dfa10002-...` | write | `[cmd][arg]` |
| Image | `dfa10003-...` | notify | JPEG stream |
| Status | `dfa10004-...` | read | text line: sensor, size, counters, free PSRAM |

Commands: `01` capture and stream, `02 n` size (0 QVGA, 1 VGA, 2 SVGA, 3 XGA, 4 SXGA, 5 UXGA),
`03 q` JPEG quality (4 best .. 63 smallest), `10 0|1` LED, `11 0|1` IR light.

Image packets (little-endian): `01 len32 w16 h16` start, `02 seq16 data...` data,
`03 crc32` end (zlib CRC32 of the whole JPEG).

### Troubleshooting

- `cam_hal: FB-OVF` or garbled frames: lower *XCLK* in menuconfig (try 8 MHz) and check the flex cable.
- Image upside down: toggle *Flip image vertically* in menuconfig.
- `PSRAM: 0 bytes`: the octal PSRAM options in `sdkconfig.defaults` were not applied
  (delete `sdkconfig` and rebuild, or `idf.py fullclean`).
- LED / IR polarity is assumed active-high; if they look inverted, swap the levels in `ble_service.c`.
