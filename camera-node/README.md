# camera-node

Battery powered camera node for the DFRobot DFR1154 (ESP32-S3 AI CAM: ESP32-S3R8, 16 MB flash,
8 MB octal PSRAM, OV3660, LTR-308 light sensor), ESP-IDF v6.1.

The node sleeps. When an external event pulls its wake-up line (GPIO10) it wakes, checks the light,
switches the IR LEDs on if it is dark, takes **one** picture, sends it over Bluetooth LE to a paired
computer and goes back to sleep.

## Contents

1. [What happens, step by step](#what-happens-step-by-step)
2. [Build and flash](#open-the-dev-container) (dev container, plain Docker)
3. [First run: pairing and receiving pictures](#first-run-pairing-and-receiving-pictures)
4. [How BLE works here (for newcomers)](#how-ble-works-here-for-newcomers)
5. [Sleep and power](#sleep-and-power)
6. [Configuration](#configuration)
7. [Source reference: every module and function](#source-reference)
8. [BLE protocol](#ble-protocol)
9. [Extending it](#extending-it)
10. [Troubleshooting](#troubleshooting)
11. [Assumptions that are not verified on hardware](#assumptions-not-verified-on-hardware)

## What happens, step by step

```
 power-up / reset (once)                       event cycle (every wake-up)
 ------------------------                      ----------------------------
 1  configuration (hard-coded or flash)        6  wake-up event detected (GPIO10)
 2  start camera -> standby                    7  check ambient light (LTR-308)
 3  start BLE (self-test) -> stop              8  too dark -> IR LEDs on
 4  start light sensor -> standby              9  capture one frame
 5  light sleep until GPIO10 event             10 IR LEDs off
    (if nobody is paired yet: a pairing        11 BLE: advertise, wait for the paired computer
     cycle runs first, see below)              12 send the frame, wait for the computer's ACK
                                               13 power everything down
                                               14 light sleep until GPIO10 event  -> back to 6
```

Every step prints a log line `[STEP nn] text (+ms)` with the milliseconds since the cycle started, so you can
see exactly where time goes. All logs of the application are controlled by **one flag**,
`LOGS_ENABLED` in `main/node_log.h` (see [Configuration](#configuration)).

Two design choices worth knowing:

* **Light sleep, not deep sleep.** RAM and CPU state survive, so after a wake-up the program simply
  continues; nothing is booted or re-configured. (Deep sleep would use less current but reboots on every
  event; see [Sleep and power](#sleep-and-power).)
* **The Bluetooth stack is shut down after every cycle.** A running Bluetooth controller keeps the radio
  subsystem alive; it is started when a picture has to be sent and completely stopped afterwards. The
  pairing keys are kept in flash (NVS), so this costs no re-pairing.

Step 3 at power-up is only a self-test: it starts the stack once, reads how many computers are paired and
stops it again.

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

## First run: pairing and receiving pictures

The node can only deliver pictures to a computer that is **listening** when the node wakes, so run the
receiver on the computer first.

1. **Receiver (computer).** Install [uv](https://docs.astral.sh/uv/) and run
   ```bash
   uv run tools/receiver.py
   ```
   It scans for `DFR1154-CAM`, connects when the node appears, pairs the first time, saves each JPEG into
   `captures/` and sends an ACK so the node can sleep at once. Then it scans again, forever.
   (`--once` stops after one picture, `--no-pair` skips the pairing call and relies on the OS.)
2. **Power-up = pairing window.** When the node starts and nobody is paired yet (or after a factory reset)
   it runs one cycle immediately and advertises for `NODE_BLE_PAIRING_WINDOW_S` (60 s by default) so the
   receiver can connect and pair. You should see `[STEP 11] computer connected, paired and listening` and a
   picture arriving on the computer.
3. **Normal operation.** Pull GPIO10 to its active level (default: drive it HIGH). The node wakes, takes a
   picture and the receiver stores it. Wait until the line is idle again before the next event (wake-up is
   level triggered, see [Sleep and power](#sleep-and-power)).
4. **Factory reset** (forget the paired computer and stored config): reset the node, then press and hold
   **BOOT** for 1 s within the 1.5 s that follows (`NODE_RESET_BUTTON_*` in `main/node_config.h`).
   Holding BOOT *during* the reset would start the ROM download mode instead.
   `idf.py erase-flash` also resets everything.

`tools/ble_viewer.html` is a manual alternative (Chrome/Edge, Web Bluetooth) for the pairing window.

On Windows you can also pair through *Settings -> Bluetooth & devices -> Add device* while the node is
advertising (right after a reset); the receiver then only has to connect.

## How BLE works here (for newcomers)

* **Roles.** The node is the *peripheral*: it only advertises and accepts a connection. The computer is the
  *central*: it scans, decides to connect and drives the exchange. A peripheral cannot call a central; it
  can only advertise and wait. That is why a receiver program must be running and scanning.
* **Advertising.** After wake-up the node broadcasts "I am DFR1154-CAM" every 20-30 ms (fast, to be found
  quickly). The computer's scan sees it and connects. Time-to-connect depends mostly on the computer's
  scanning, not on the node.
* **Pairing and bonding.** The first time, both sides run a pairing procedure (LE Secure Connections,
  "Just Works" by default, or a fixed PIN, see Kconfig) that creates a shared encryption key. *Bonding*
  means both sides store that key. Later connections only need to re-encrypt with the stored key, which is
  quick. The node stores its keys in NVS flash, so they survive the stack shutdown and battery changes.
* **Why encryption matters.** The node refuses to send a picture until the link is encrypted
  (`NODE_BLE_REQUIRE_ENCRYPTION`). Without that, anyone in range could connect and receive your images.
  Note that "Just Works" pairing has no PIN: anyone who pairs *first* is accepted. Choose *Fixed passkey*
  in menuconfig (and change the PIN) if that matters to you.
* **GATT.** Data is organised in a *service* with *characteristics*. Ours has two: **Image** (the node
  pushes data with *notifications* after the computer subscribed) and **Control** (the computer writes a
  one-byte ACK/NAK back).
* **Throughput.** A notification carries at most `MTU - 3` bytes. The computer negotiates the MTU (up to
  512 here); the node also asks for long link-layer packets and a 7.5-15 ms connection interval.
  A picture is cut into chunks of `MTU - 6` bytes (3 bytes ATT header + 3 bytes of our own header).
* **ACK.** After the last packet the computer confirms the frame (checksum verified) so the node can
  disconnect immediately instead of waiting a safety time. No ACK within `NODE_BLE_ACK_TIMEOUT_MS`
  (2 s) means the node assumes delivery and disconnects anyway.

## Sleep and power

**Light sleep.** `power_sleep_until_event()` calls `esp_light_sleep_start()`. CPU and RAM are kept (frame
buffers in PSRAM, variables, FreeRTOS state), there is no boot and no configuration: execution simply continues on the next line after the wake-up.

**Wake-up line.** GPIO10 (the SD_CS pin of the unused microSD socket; keep the socket empty). It uses an
internal pull-down (active HIGH, default) or pull-up (active LOW, `NODE_WAKE_ACTIVE_LOW`). Light-sleep
wake-up is **level triggered**:

* the line must go back to idle before the node can sleep again; `power_sleep_until_event()` polls with
  200 ms timer sleeps while the line is still active so it cannot wake in a loop;
* an event that starts *and ends* while the node is busy (a cycle takes seconds) is missed; a sensor that
  holds its output for a while (a PIR does) is fine.

**What is switched off before sleeping**

| Part | State while sleeping |
|---|---|
| IR LEDs | off, pin latched with `gpio_hold_en` so it cannot float |
| Camera sensor | software power-down (register 0x3008 bit 6); XCLK stops with the clock gating of light sleep |
| Light sensor | standby (`MAIN_CTRL` = 0) |
| BLE | disconnected, advertising stopped, controller and host fully deinitialised |
| CPU / RAM | light sleep (RAM kept) |

The DFR1154 does not connect the camera's PWDN pin (`CAM_PIN_PWDN = -1`) and has no power switch for the
sensor, so the sensor's own standby current remains. Measure the real consumption of your board before
optimising further.

**Speed.** Where the time goes in a cycle is visible in the `(+ms)` column of the log. Typical levers:

* `NODE_SETTLE_FRAMES_DAY/NIGHT` in `main/node_config.h`: frames thrown away after waking the sensor so the
  auto exposure settles. Fewer = faster but darker/brighter first frame.
* `NODE_CAMERA_XCLK_MHZ`: a higher camera clock means faster frames (10 MHz is known-good on this board;
  try 16-20 and watch for `FB-OVF`).
* Frame size and JPEG quality: smaller files are sent faster over BLE.
* `LOGS_ENABLED 0`, bootloader and default log level "No output": less UART time and a faster cold boot.
* Parallel BLE: starting `ble_link_start(true)` *before* the capture would hide the connection time behind
  the exposure settling. It is not done by default because the IR LEDs, camera and radio would then draw
  current at the same time, which is risky on a battery.

**Deep sleep** (not implemented) would use less sleep current but reboots the chip on every event
(about a second of boot, camera init and PSRAM init before the first frame). If you need it, the code is
structured for it: run steps 1-4 on every boot, run `run_cycle()` when the wake cause is the GPIO, then
call `esp_deep_sleep_start()` with `esp_sleep_enable_ext1_wakeup_io()` on GPIO10 (an RTC pin).

## Configuration

**Hard-coded settings (step 1)** are `#define`s at the top of `main/node_config.h`:

| Define | Default | Meaning |
|---|---|---|
| `NODE_DEFAULT_FRAME_SIZE_IDX` | 2 | 0 QVGA, 1 VGA, **2 SVGA 800x600**, 3 XGA, 4 SXGA, 5 UXGA |
| `NODE_DEFAULT_JPEG_QUALITY` | 12 | 8 (best) .. 63 (smallest). Below 8 the JPEG can exceed the driver's frame buffer |
| `NODE_DEFAULT_LIGHT_THR` | 30 | 0 (dark) .. 100 (bright), log scale of lux: 0 = 0 lux, 30 = about 26 lux, 50 = about 220 lux, 100 = about 50 000 lux. Below the threshold = night |
| `NODE_SETTLE_FRAMES_DAY/NIGHT` | 3 / 6 | frames discarded before the real capture |
| `NODE_IR_WHEN_LIGHT_UNKNOWN` | 1 | if the light sensor cannot be read: assume dark (IR on) |
| `NODE_STORE_DEFAULTS_TO_FLASH` | 0 | 1 = write the defaults to flash on first boot (tests the persistence path) |
| `NODE_RESET_BUTTON_WINDOW_MS/HOLD_MS` | 1500 / 1000 | factory-reset gesture; window 0 disables it (and removes the start-up wait) |

**Stored configuration.** `node_config_load()` uses the copy in flash when its *configured flag* is set
and valid, otherwise the hard-coded defaults. Nothing writes to flash yet (that is the future BLE setup);
call `node_config_save()` from your setup code to store a complete configuration and set the flag.
From then on the stored values win after every reset, including after the battery ran flat, until
`node_config_erase()` (factory reset). *Consequence for development:* once a config is stored, editing the
`#define`s no longer changes anything; erase it with the BOOT gesture or `idf.py erase-flash`.

**One flag for logs.** `#define LOGS_ENABLED 1` in `main/node_log.h`. With `0` every `NLOGx`/`NSTEP` call
compiles to nothing and `app_main` mutes all IDF logging at start-up. For the leanest build also set
the bootloader and default log level to *No output* in menuconfig.

**menuconfig** (`idf.py menuconfig` -> *Camera node*):

| Option | Default | Meaning |
|---|---|---|
| `NODE_WAKE_GPIO` | 10 | wake-up input |
| `NODE_WAKE_ACTIVE_LOW` | off | event = line goes LOW (internal pull-up) instead of HIGH |
| `NODE_CAMERA_XCLK_MHZ` | 10 | camera clock |
| `NODE_CAMERA_VFLIP` / `HMIRROR` | on / off | image orientation |
| `NODE_BLE_DEVICE_NAME` | DFR1154-CAM | advertised name (the receiver matches the prefix `DFR1154`) |
| `NODE_BLE_CONNECT_TIMEOUT_MS` | 10000 | how long a normal cycle waits for the computer |
| `NODE_BLE_PAIRING_WINDOW_S` | 60 | how long the power-up pairing cycle waits |
| `NODE_BLE_ACK_TIMEOUT_MS` | 2000 | wait for the computer's ACK |
| `NODE_BLE_REQUIRE_ENCRYPTION` | on | only send over a paired link |
| `NODE_BLE_PAIRING_METHOD` | Just Works | or fixed passkey (`NODE_BLE_PASSKEY`) |

After changing options with `idf.py menuconfig`, run `idf.py save-defconfig` and commit the updated
`sdkconfig.defaults`. It additionally enables NimBLE with NVS persistence for bonds, a 1 ms FreeRTOS tick,
the PSRAM at 80 MHz and disables the PSRAM self-test. After changing it delete `sdkconfig` (or run
`fullclean`) so the new defaults are picked up.


## Source reference

Everything is plain C in `main/`. Naming: `camsvc_*` camera, `als_*` light sensor, `board_*` board IO,
`power_*` sleep, `ble_link_*` Bluetooth, `node_*` logging/config. (The camera functions are *not* called
`cam_*`: the esp32-camera library already exports `cam_init` and friends.)

### `camera_node.c` - the flow

| Function | What it does |
|---|---|
| `app_main()` | Runs once after power-up/reset. Mutes IDF logs if `LOGS_ENABLED` is 0, initialises NVS, checks the BOOT factory-reset gesture, then steps 1-4 (load config, `camsvc_init`, BLE self-test, `als_init`), configures the wake input, runs a pairing cycle if nobody is paired, and finally loops forever: `power_sleep_until_event()` then `run_cycle(false)` (steps 5/14 and 6-13). If light sleep itself fails it waits 500 ms instead of spinning. |
| `run_cycle(pairing_window)` | One complete event cycle, steps 6-13, with the log line of every step. Wakes camera and light sensor, measures light, switches IR, captures, switches IR off, puts camera and sensor back to standby *before* starting BLE (so the radio never runs together with the IR LEDs or the sensor), then starts BLE, waits for the computer (`NODE_BLE_CONNECT_TIMEOUT_MS`, or the pairing window), sends the frame (at most 2 attempts if the computer answers NAK), stops BLE and releases the frame buffer. If the capture failed nothing is sent. |
| `is_dark(cfg)` | Step 7. Reads the light sensor (up to 200 ms), converts lux to the 0-100 level and compares it with `cfg->light_threshold`. If the sensor cannot be read the result is `NODE_IR_WHEN_LIGHT_UNKNOWN`. |

### `node_log.h/.c` - logging

| Item | What it does |
|---|---|
| `LOGS_ENABLED` | The single on/off switch (see Configuration). |
| `NLOGI/NLOGW/NLOGE(tag, fmt, ...)` | Like `ESP_LOGI/W/E`; compile to nothing when logs are off. Use these, not `ESP_LOGx`, in new code. |
| `NSTEP(num, fmt, ...)` | A numbered flow step: prints `[STEP nn] text (+ms)`. |
| `node_log_cycle_start()` | Sets the time zero used by `(+ms)`; called at the start of `app_main` and of every cycle. |
| `node_log_elapsed_ms()` | Milliseconds since that point; also works with logs off (used for the "cycle finished in" line). |
| `node_log_step()` | Implementation behind `NSTEP` (printf into a 160 byte buffer). |

### `node_config.h/.c` - settings and the flash flag

| Function | What it does |
|---|---|
| `node_config_load()` | Starts from the hard-coded defaults; if NVS namespace `node`, key `cfg` holds a blob with the right magic/version, `configured == 1` and plausible values, those replace the defaults. Optionally stores the defaults when `NODE_STORE_DEFAULTS_TO_FLASH` is 1. Logs where the settings came from. |
| `node_config_get()` | Pointer to the settings in use. |
| `node_config_is_from_flash()` | `true` if they came from flash. |
| `node_config_save(cfg)` | Validates and writes the blob with `configured = 1`, then uses it. This is the hook for the future BLE setup. |
| `node_config_erase()` | Deletes the blob and returns to the defaults. |
| `valid()` / `use_defaults()` | Range check (size index, quality 8-63, threshold 0-100) / reset RAM copy. |
| `stored_t` | Blob layout `{magic, version, configured, node_config_t}`. Bump `VERSION` when you change it; an old blob is then ignored and the defaults are used. |

### `camera_service.h/.c` - OV3660

The camera is initialised once and then parked in **sensor software standby** (OV3660 register `0x3008`
bit 6). Registers are kept, so waking is a register write plus a short delay.

| Function | What it does |
|---|---|
| `camsvc_init(cfg)` | Full `esp_camera_init`: JPEG, 2 frame buffers in PSRAM, `CAMERA_GRAB_LATEST`. **Initialises one frame size larger than wanted and then switches down**: the driver sizes its JPEG buffers from the init size and never resizes them, so a buffer that is too small gives `cam_hal: FB-OVF`. Applies flip/mirror, brightness/saturation, frame size and quality, then calls `camsvc_standby()`. |
| `camsvc_reinit(cfg)` | `esp_camera_deinit` + `camsvc_init`. Used for recovery and when the settings change. |
| `camsvc_standby()` | Sets the software power-down bit. |
| `camsvc_wake()` | Clears it and waits 10 ms. If the camera was never initialised it does a full init first. |
| `camsvc_capture(settle_frames)` | Discards `settle_frames` frames (auto-exposure convergence and stale frames from before the standby), then returns the next frame if it is a complete JPEG (starts `FFD8`, ends `FFD9`). If frames stop arriving or the JPEG is incomplete it re-initialises the camera once and retries (the slow recovery path). Returns `NULL` on failure. |
| `camsvc_release(fb)` | Gives the frame buffer back to the driver. Always release what you captured. |
| `camsvc_size_name(idx)` | `"SVGA 800x600"` etc. for logs. |
| `is_complete_jpeg()` | The marker check used above. |

### `als.h/.c` - LTR-308 ambient light sensor

Address `0x53` on the camera's SCCB bus (GPIO8/9). It attaches to the bus the camera driver already
created (`i2c_master_get_bus_handle`), so initialise it **after** the camera. Configured for 16 bit /
25 ms / gain 3x: the first sample is ready about 30 ms after waking, range about 0.8 lux to 52 000 lux.

| Function | What it does |
|---|---|
| `als_init()` | Finds the camera's I2C bus, scans it once and logs the addresses found (camera `0x3c`, sensor `0x53`), adds the device, reads the part id (`0xB1`), writes the configuration and puts the sensor in standby. |
| `als_available()` | `true` if `als_init()` succeeded. |
| `als_wake()` / `als_standby()` | Writes measurement rate, gain and `MAIN_CTRL` (active / standby). The configuration is rewritten on every wake. |
| `als_read_lux(&lux, timeout_ms)` | Polls `MAIN_STATUS` for the new-data flag, reads the 20-bit value and returns `0.6 * raw / (gain * integration)` lux. |
| `als_lux_to_level(lux)` | `log10(lux + 1) / 4.7 * 100`, clamped to 0-100. This is the unit of `light_threshold`. |
| `reg_read()` / `reg_write()` | One-register I2C helpers. |

### `board_io.h/.c` - IR LEDs and BOOT button

| Function | What it does |
|---|---|
| `board_ir_set(on)` | Drives GPIO47 (`IR_CT`), configuring the pin on first use. Uses `gpio_reset_pin` / `gpio_set_direction` rather than `gpio_config()` with a mask: during development a `gpio_config()` call that included GPIO47 ended in a silent watchdog reset on this board/IDF combination. |
| `board_ir_get()` | Last commanded state. |
| `board_prepare_sleep()` | IR off and `gpio_hold_en` so the pin cannot float in light sleep. |
| `board_resume()` | `gpio_hold_dis` after waking. |
| `board_boot_button_held(window_ms, hold_ms)` | Polls GPIO0 for `window_ms`; true as soon as it was low for `hold_ms` in a row. |

### `power.h/.c` - sleep

| Function | What it does |
|---|---|
| `power_wake_gpio_init()` | Makes `NODE_WAKE_GPIO` an input with pull-down (active high) or pull-up (active low), also configured for the sleep state. |
| `power_wake_pin_active()` | Level check. |
| `power_sleep_until_event()` | 1) while the line is still active: sleep in 200 ms timer slices; 2) arm GPIO wake-up on the active level; 3) `board_prepare_sleep()`; flush the console; 4) `esp_light_sleep_start()`; 5) `board_resume()`. Returns `true` if the wake cause was the GPIO (or the sleep was rejected because the event arrived at that very moment), `false` on any other result. |
| `flush_console()` | Waits until the UART has sent everything, so the last log lines are not cut off by sleep. |

### `ble_link.h/.c` - Bluetooth

Public API, in the order of a normal cycle:

| Function | What it does |
|---|---|
| `ble_link_erase_bonds()` | Erases the NimBLE key store (NVS namespace `nimble_bond`). Only while the stack is stopped. |
| `ble_link_start(advertise)` | `nimble_port_init`, security settings (LE Secure Connections + bonding, Just Works or fixed passkey), registers the GATT service, opens the key store (`ble_store_config_init`) and starts the host task. With `advertise = true` advertising starts as soon as the stack is synced. Everything is rebuilt on every call, so it can be called once per cycle. |
| `ble_link_wait_synced(timeout)` | Blocks until the stack knows its address (used by the boot self-test). |
| `ble_link_bond_count()` | Number of stored pairings. |
| `ble_link_wait_ready(timeout)` | Blocks until a computer is connected, the link is encrypted (if required) and the Image characteristic is subscribed. Logs which of the three is missing on timeout. |
| `ble_link_send_image(jpeg, len, w, h)` | Streams START, DATA... and END (with CRC32) as notifications, chunk size `MTU - 6`. Aborts if the connection drops. |
| `ble_link_wait_ack(timeout)` | `BLE_ACK_OK`, `BLE_ACK_NAK` or `BLE_ACK_TIMEOUT`. |
| `ble_link_stop()` | Disconnects (waits up to 0.5 s), stops advertising, `nimble_port_stop` + `nimble_port_deinit`. Safe to call when not running. |

Internal functions:

| Function | What it does |
|---|---|
| `start_advertising()` | Name in the advertising data, 128-bit service UUID in the scan response, connectable, 20-30 ms interval. |
| `gap_event()` | Central event handler. *Connect*: stores the handle, asks for 251-byte packets and a 7.5-15 ms interval, sends a security request. *Disconnect*: clears the state and, while a cycle is running, advertises again so the computer can reconnect. *Subscribe*: remembers that notifications are on. *Enc change*: link encrypted. *Repeat pairing*: deletes the old bond and retries (the computer forgot its key). *Passkey action*: injects the fixed PIN (passkey mode only). *MTU*: logs. |
| `on_sync()` / `on_reset()` | Stack ready (address, start advertising) / stack reset. |
| `host_task()` | Runs the NimBLE host until `nimble_port_stop()`. |
| `ctrl_access()` / `image_access()` | GATT callbacks: Control accepts the 1-byte ACK/NAK; Image is notify-only. |
| `notify_retry()` | One notification; if the buffers are full it waits 2 ms and retries (up to about 4 s). |

### `board_dfr1154.h` - pin map

Camera DVP pins, `BOARD_PIN_IR` (47), `BOARD_PIN_LED` (3, unused), `BOARD_PIN_BOOT` (0),
`BOARD_PIN_LTR_INT` (48, the light sensor interrupt, unused). All verified against the board schematic.
GPIO35-37 belong to the octal PSRAM; never use them.

### `tools/`

| File | Purpose |
|---|---|
| `receiver.py` | Unattended receiver (Python + bleak, run with `uv run`). `Assembler` rebuilds the JPEG from the packets and checks the CRC; `session()` handles one connection (connect, pair, subscribe, save, ACK/NAK); `main()` scans forever. |
| `ble_viewer.html` | Manual receiver for the pairing window (Chrome/Edge). |

## BLE protocol

Service `dfa10001-5b3c-4f6d-9e21-0a4c7b8d1e00`, all multi-byte fields little-endian.

| Characteristic | UUID | Properties | Content |
|---|---|---|---|
| Control | `dfa10002-5b3c-4f6d-9e21-0a4c7b8d1e00` | write | `01` = frame received OK (ACK), `02` = resend (NAK) |
| Image | `dfa10003-5b3c-4f6d-9e21-0a4c7b8d1e00` | notify | packets below |

| Packet | Layout |
|---|---|
| START | `01` `length u32` `width u16` `height u16` |
| DATA | `02` `seq u16` `jpeg bytes...` (seq starts at 0) |
| END | `03` `crc32 u32` (zlib CRC32 of the whole JPEG) |

The receiver subscribes, collects START/DATA/END, verifies length and CRC, writes ACK or NAK.

## Extending it

* **Setup by BLE (planned).** Add a writable, encrypted *Config* characteristic in `s_gatt_svcs`
  (`ble_link.c`) and a handler that validates the bytes and calls `node_config_save()`. Because the
  stack is only up during a cycle, run a dedicated "setup cycle" (for example triggered by the BOOT
  gesture) that stays connected longer. Frame size or quality changes take effect after
  `camsvc_reinit(node_config_get())`, because the frame buffers are sized at init.
* **Another wake source.** Change `NODE_WAKE_GPIO` / polarity in menuconfig. For a timer wake-up add
  `esp_sleep_enable_timer_wakeup()` in `power_sleep_until_event()` and accept `ESP_SLEEP_WAKEUP_TIMER`.
* **More data with the picture** (battery voltage, lux): add a packet type to `ble_link_send_image()`
  and the receiver, keep START/DATA/END unchanged for existing receivers.
* **A new step in the cycle:** add it in `run_cycle()` with `NSTEP(n, ...)` so it shows up in the timing log.

## Troubleshooting

| Symptom | Likely cause / what to do |
|---|---|
| Boot log shows `esp_camera_init failed` | Camera cable or pins; `cam_hal: FB-OVF` or garbled frames: lower `NODE_CAMERA_XCLK_MHZ`. |
| `I2C devices on bus:` lacks `0x53` | Light sensor not reachable; the node then uses `NODE_IR_WHEN_LIGHT_UNKNOWN`. Send me the `als:` log lines. |
| Node wakes again and again | The wake line stays active. Check polarity (`NODE_WAKE_ACTIVE_LOW`) and that the line returns to idle. The log shows `wake line still active`. |
| Nothing wakes the node | Wrong polarity, or the pin is not driven; an empty microSD socket must be left empty. Check with a wire to 3V3. |
| `no usable computer within ... ms` | The receiver is not running or did not pair. The log tells which of connected / encrypted / subscribed is missing. |
| Connected but never encrypted | Pairing did not finish. Pair via the OS Bluetooth settings during the pairing window, or run a factory reset on both sides (remove the device from the OS list, BOOT gesture on the node). |
| Picture arrives but `CRC MISMATCH` | Packet loss; the receiver sends NAK and the node resends once. If frequent: smaller frame / lower quality, move closer. |
| Pictures are dark | Raise `NODE_SETTLE_FRAMES_NIGHT/DAY` or the light threshold. |
| `capture failed ... re-initialising the camera` | The fast wake path did not work after sleep; the node recovered through the slow path. Tell me, then the standby approach can be replaced by a re-init per cycle. |
| Stuck after changing `#define` settings | A configuration stored in flash wins over the defaults: factory reset with BOOT. |
| `Permission denied: '/workspace/sdkconfig'` | Dev container and PowerShell/Docker builds mixed (root vs `dev` user). Delete `build`, `sdkconfig`, `sdkconfig.old` and use one build method. |

## Assumptions not verified on hardware

This project was written without access to the board or to an ESP-IDF 6.1 toolchain; the C code was only
syntax-checked against stub headers. Please treat the following as the first things to verify:

1. **It compiles against the real IDF 6.1 headers** (NimBLE security API, `i2c_master_*`, GPIO sleep calls).
2. **Light sleep with the camera parked in standby.** The OV3660 power-down bit (`0x3008`, bit 6) is taken
   from the OV5640/OV3660 register map. After wake-up the capture DMA must still deliver frames; if not,
   `camsvc_capture()` falls back to a re-init (slower, logged as a warning).
3. **LTR-308 register values** (config `0x40`, gain `0x01`, 25 ms integration factor 0.25) follow the
   datasheet from memory; compare the reported lux with a known light source.
4. **Wake line.** GPIO10 polarity and pull are assumptions; nothing is known about what drives it.
5. **Repeated `nimble_port_init` / `nimble_port_deinit`** once per cycle, and the key store namespace name
   `nimble_bond` used by `ble_link_erase_bonds()`.
6. **Pairing with `receiver.py` on your OS.** `bleak`'s `pair()` is supported on Windows and Linux (Linux
   needs a pairing agent), not on macOS; fixed-passkey pairing cannot be typed by `bleak` on Windows.
7. **No current figures were measured.** Check the sleep current and the current peak while the IR LEDs, the
   camera or the radio are active, especially with a weak battery (brown-out resets show up as
   `rst:0xf (BROWNOUT_RST)`).
