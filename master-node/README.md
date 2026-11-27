# master-node

Zephyr v4.4.2 firmware that **wakes the camera node, receives its picture over
Bluetooth LE and keeps it in RAM**. Developed on the **nRF5340 DK**, to be ported to the
**XIAO nRF52840** (+ Wio-SX1262 LoRa) which is the final hardware. Both boards build from
this one folder.

> Status: written without access to the Zephyr toolchain or the boards, so it has never
> been compiled or run. See "Unverified assumptions" at the end and send the first build
> error / log if something fails.

## What it provides

Two functions, declared in `src/camera_node.h`:

```c
camera_node_init();                      // once: wake pin + Bluetooth module (BLE stays off)

if (take_image()) {                      // wake the camera, receive its picture, BLE off
    const struct stored_image *img = get_image();   // picture in RAM: img->data, img->len, ...
    /* ... hand it to the next stage (LoRa etc. - not part of this code) ... */
}
```

`read_battery(&info)` - asks the camera node for its battery level: a **long** wake pulse (`APP_BATTERY_WAKE_PULSE_MS`,
300 ms; the camera treats pulses longer than its `NODE_BATTERY_REQUEST_MS` = 200 ms as a battery request), then it receives the
battery report over BLE, logs `BATTERY: 73 % (7620 mV battery, 1905 mV at the camera's ADC pin)`, turns Bluetooth off and returns
`true` with `info->percent`, `battery_mv`, `adc_mv`. Needs a paired camera (call `take_image()` once first).

`take_image()` (uses a **short** pulse, `APP_IMAGE_WAKE_PULSE_MS` = 50 ms):
1. switches Bluetooth on (`bt_enable`, loads the stored pairing from flash),
2. pulls the camera's wake line LOW for 100 ms,
3. scans for the paired camera, connects, encrypts with the stored key, receives the
   picture into RAM (logging the saved bytes) and sends the ACK,
4. switches Bluetooth off (`bt_disable`),
5. returns `true` if a complete, CRC-checked picture is available through `get_image()`.

`get_image()` returns the picture (pointer + length + size + `status` (`IMAGE_FACE` / `IMAGE_HUMAN` / `IMAGE_NO_HUMAN`) and `human_count` / `face_count`, as reported by the camera) or `NULL`. The data stays valid until
the next `take_image()`; do not call it while `take_image()` runs.

`src/main.c` is only an example caller: it sleeps until the trigger input (Button 1 on the DK)
wakes the nRF and calls `take_image()` + `get_image()`; **Button 2** calls `read_battery()` and logs the result. Replace it with the product's own
main loop; the trigger code is the only other thing in there.

Details:
* **Wake line**: `cam-wake-gpios` is pulled LOW for 100 ms, which wakes the camera node from
  light sleep (its `NODE_WAKE_ACTIVE_LOW` option must be ON).
* **Sleep**: between calls the CPU sleeps in the Zephyr idle thread (System ON, RAM kept). The
  radio is off outside `take_image()`. System OFF is not used because it would erase the picture.
* **Pairing**: with no stored pairing, `take_image()` runs in pairing mode: it wakes the camera and
  pairs ("Just Works", LE Secure Connections) with the first camera named `APP_CAMERA_NAME`. Both sides
  store the bond (nRF in flash through Zephyr settings, ESP32 in NVS). From then on only the paired
  camera is accepted (address must match). So the first `take_image()` call after flashing does the
  pairing and also delivers the first picture.
* **Forgetting**: `camera_node_forget_pairing()`; reset the camera node as well (reset, then hold
  BOOT 1 s), otherwise `APP_AUTO_REPAIR` (default on) cleans up on the next call.
* **Storage**: one static RAM buffer (`APP_IMAGE_BUF_SIZE`, 128 KB). A picture becomes visible only
  after its size and CRC32 matched.

Expected log of a good run:

```
master_node: sleeping until the trigger input is activated
camlink: Bluetooth ON, own address ..., 1 paired camera(s)
camera_node: take_image: paired camera
cam_wake: waking the camera node (100 ms pulse)
camlink: listening for "DFR1154-CAM" (paired camera only), up to 20000 ms
camlink: camera found: XX:XX:.. (public), RSSI -48 dBm -> connecting
camlink: connected, waiting for pairing / encryption
camlink: link encrypted (security level 2)
camlink: ATT MTU 247 (status 0)
camlink: service found: image handle 12, control handle 10
camlink: subscribed - waiting for the picture
camlink: incoming picture 800x600, 41234 bytes
camlink: saved 8192 / 41234 bytes
...
camlink: TRANSMISSION END: 41234 bytes saved in RAM, CRC32 1a2b3c4d OK, 1850 ms (22288 B/s)
camlink: ACK sent to the camera
camlink: disconnected (reason 0x13)
camlink: session over: OK
camlink: Bluetooth OFF
camera_node: take_image: picture ready, 41234 bytes (800x600)
master_node: image ready: 41234 bytes
```

## Wiring (nRF5340 DK <-> camera node DFR1154)

| nRF5340 DK                | Camera node              |
|---------------------------|--------------------------|
| P1.04 (`cam-wake-gpios`)  | GPIO10 (wake input; short LOW pulse = picture, long = battery) |
| GND                       | GND                      |

The camera's GPIO10 has an internal pull-up and wakes on LOW. The DK drives push-pull
(HIGH when idle, LOW for the pulse), so no extra parts are needed. Check P1.04 against the DK
pin table before wiring; any free GPIO works, change it in
`boards/nrf5340dk_nrf5340_cpuapp.overlay`. Both boards run at about 3 V, so levels are compatible.
Camera node settings: `NODE_WAKE_ACTIVE_LOW=y`, `NODE_BLE_DEVICE_NAME` = `APP_CAMERA_NAME`
(default `DFR1154-CAM`), `NODE_BLE_PASSKEY` pairing mode off (Just Works).

## Folders and files

| File                                       | What                                                           |
|--------------------------------------------|----------------------------------------------------------------|
| `west.yml`                                 | Pins Zephyr and the modules we fetch                           |
| `prj.conf`                                 | Kconfig: logging, Bluetooth central, bonding, MTU, CRC         |
| `Kconfig`                                  | Application options (`CONFIG_APP_*`)                           |
| `boards/nrf5340dk_nrf5340_cpuapp.overlay`  | DK: Button 1 = picture, Button 2 = battery, wake pin = P1.04                       |
| `boards/xiao_ble.overlay` / `.conf`        | XIAO: LoRa wiring, placeholder trigger/wake pins, LoRa on      |
| `Kconfig.sysbuild`, `sysbuild.cmake`       | nRF5340 only: also builds the network-core Bluetooth controller|
| `sysbuild/hci_ipc.conf`                    | Options for that network-core image                            |
| `src/camera_node.c/.h`                     | **Public API**: `take_image()`, `get_image()`                  |
| `src/main.c`                               | Example caller: trigger input, loop                            |
| `src/camera_link.c/.h`                     | The whole BLE session (scan -> pair -> receive -> ACK)         |
| `src/image_store.c/.h`                     | RAM picture buffer                                             |
| `src/cam_wake.c/.h`                        | Wake line to the camera node                                   |

## Build

Open the folder in the dev container (see below), then:

```bash
# nRF5340 DK: two images (app core + network core), needs --sysbuild
west update --narrow -o=--depth=1
west build -b nrf5340dk/nrf5340/cpuapp --sysbuild -p

# XIAO nRF52840 (final hardware): single image
west update --narrow -o=--depth=1
west build -b xiao_ble -p
```

Use `-p` (pristine) when you switch boards. The two targets use separate board files, so
the XIAO build never sees the DK parts and vice versa.

### Dev container

Unchanged from before (`.devcontainer/`): Docker + VS Code Dev Containers,
"Reopen in Container". The first start downloads Zephyr and modules into a volume. If
you already have the container, rebuild it only if `west update` complains about missing
modules (this project adds `tinycrypt`, `mbedtls`, `tf-psa-crypto`, `tf-psa-crypto-drivers`, `open-amp` and `libmetal` to `west.yml`; after
changing `west.yml` run `cd /workdir && west update --narrow -o=--depth=1`).

## Flash

### nRF5340 DK (from the host, USB cable to the DK's J-Link port)

The container has no USB access, so flash from the host. The DK has two cores, so there are **two hex files**
(find them with `find build -name zephyr.hex`):

```
build/hci_ipc/zephyr/zephyr.hex        network core (Bluetooth controller)
build/master-node/zephyr/zephyr.hex    application core (this firmware)
```

**nRF Connect Programmer** (nRF Connect for Desktop, tested version 4.6.2): select the DK in the device list,
**Add file** -> pick both hex files, check that the memory layout shows the network-core and application-core
regions, click **Write** (tick *Erase all* if the first attempt is refused), then reset the board.

**nRF Util** (command line):

```bash
nrfutil device program --firmware build/hci_ipc/zephyr/zephyr.hex     --core Network
nrfutil device program --firmware build/master-node/zephyr/zephyr.hex --core Application
nrfutil device reset
```

If a protection error appears, run `nrfutil device recover --core Network` and
`nrfutil device recover --core Application` once. The J-Link drive that appears on the host also accepts `.hex` files
by drag and drop, but flashing both cores that way is less reliable.

Console: the DK shows a serial port (115200 8N1), usually the first of the two J-Link ports.

### XIAO nRF52840

Double-tap reset, copy `build/zephyr/zephyr.uf2` onto the new drive. The console is the board's USB serial port:
Windows: COM port from Device Manager, open with PuTTY (115200); Linux: `screen /dev/ttyACM0 115200`.
Messages printed before the port is opened are lost.

## First run (pairing)

1. Flash and wire the DK as above, open the DK's serial console.
2. Press Button 1. With no stored pairing it logs `no camera paired yet: pairing mode`, wakes the
   camera node (which advertises for about 10 s after each wake-up) and pairs with it. The nRF logs
   `pairing complete, bonded: yes`, the picture follows in the same session.
3. From then on each press takes a picture from the paired camera only.

If the camera node was just powered up with no bond it also advertises for 60 s on its own, so
you can press Button 1 inside that window instead.

## Source reference (what every function does)

### `src/camera_node.c` (the public API)

* `camera_node_init()` - initialises the link module and the wake pin. Bluetooth stays off.
* `take_image()` - radio on, decide pairing mode (no bond) or paired mode, wake pulse, run the
  session, one automatic re-pair if the camera lost the bond, radio off, copy the picture handle.
* `read_battery(&info)` - radio on, long wake pulse, one session, expects a battery report (`camlink_last_kind()`), radio off.
* `get_image()` - pointer to the last valid picture or `NULL`.
* `camera_node_forget_pairing()` - deletes the stored bond (radio on briefly).

### `src/main.c` (example caller, replaceable)

* `trigger_isr()` - interrupt on the trigger input, ignores edges closer than 300 ms (bounce),
  releases `trigger_sem`.
* `trigger_init()` - configures the input from `zephyr,user trigger-gpios`, arms the interrupt.
* `main()` - init, then loop: sleep on the semaphore, `take_image()`, `get_image()`.

### `src/camera_link.c`

The session is a chain of callbacks (all run in the Bluetooth thread); `camlink_receive_image()`
only starts the scan and waits on a semaphore.

* `camlink_init()` - semaphores and work items only (no radio).
* `camlink_radio_on()` - `bt_enable()`, `settings_load()` (restores bonds), registers pairing logs.
* `camlink_radio_off()` - `bt_disable()`.
* `camlink_bond_count()` / `camlink_forget_all()` - bonds in flash.
* `camlink_receive_image(pairing, timeout)` - the public entry. Scans, waits for the
  session result (`finish()`), extends the wait to `APP_TRANSFER_TIMEOUT_S` once the camera was
  found, then tears down: stop scan, wait for the camera's hang-up (or disconnect itself).
* `scan_start()` / `scan_cb()` / `ad_cb()` - passive scan at 100 % duty. A connectable advertiser
  whose name starts with `APP_CAMERA_NAME` is a candidate; in normal mode its address must
  equal the bonded address. Then `bt_conn_le_create()` (7.5-15 ms interval).
* `connected_cb()` / `disconnected_cb()` - link state. A failed connection restarts the scan.
  An unexpected disconnect ends the session with `CAMLINK_FAILED`.
* `security_changed_cb()` - the camera sends a Security Request on connect; Zephyr then encrypts with
  the stored key (or pairs if there is none). Success -> MTU exchange. `PIN_OR_KEY_MISSING` means
  the camera forgot us -> `CAMLINK_BOND_LOST`. `security_fallback_work()` starts security
  ourselves if the camera did not within 1.5 s.
* `pairing_complete_cb()` / `pairing_failed_cb()` - log pairing results.
* `start_mtu_exchange()` / `mtu_cb()` - ATT MTU (up to 247), bigger chunks -> faster transfer.
* `discover_start()` / `discover_cb()` - finds the camera service by UUID and its Image and Control
  characteristic handles, then `subscribe_start()` subscribes to Image notifications (CCC descriptor
  found automatically). The camera only sends after this subscription.
* `rx_battery()` - parses the battery report, logs it, queues the ACK; `camlink_last_kind()` / `camlink_last_battery()` return it.
* `notify_cb()` -> `rx_start()` / `rx_data()` / `rx_end()` - protocol parser:
  START (allocate in `image_store`), DATA (sequence check, append, progress log),
  END (compare size + CRC32; good -> commit + queue ACK, bad -> abort + queue NAK).
* `ack_work_fn()` - writes ACK / NAK to the Control characteristic from the system work queue
  (not from the Bluetooth thread). ACK ends the session with `CAMLINK_OK`.
* `finish()` - records the first result and releases the waiting thread.

### `src/image_store.c`

* `image_store_begin(len, w, h)` - discards the old picture, reserves `len` bytes (`-ENOSPC` if too big).
* `image_store_append(data, n)` - copy + running CRC32 (`crc32_ieee_update`, zlib compatible).
* `image_store_received/expected/crc()` - progress and checksum so far.
* `image_store_commit()` / `image_store_abort()` - mark valid / throw away.
* `image_store_get(&img)` - pointer + length + size of the last valid picture.

### `src/cam_wake.c`

* `cam_wake_init()` - output in the idle (inactive) state.
* `cam_wake_pulse()` - active for `APP_IMAGE_WAKE_PULSE_MS`, `APP_BATTERY_WAKE_PULSE_MS`, then idle.

## Protocol (same as the camera node)

Service `dfa10001-5b3c-4f6d-9e21-0a4c7b8d1e00`
* Battery report (notify on Image): `04 percent8 battery_mV16 pin_mV16`, ACKed through Control like a picture
* Control `dfa10002-...` (write): `01` = ACK picture received, `02` = please resend
* Image   `dfa10003-...` (notify): `01 len32 w16 h16 flags8 faces8 humans8` START (flags bit0 = face detected, bit1 = person
  detected; the last three bytes are optional), `02 seq16 bytes` DATA, `03 crc32` END
  (little endian; CRC32 = zlib/IEEE of the JPEG)

The camera only sends after the link is encrypted AND the Image characteristic is subscribed.

## Configuration

`west build -t menuconfig` -> "master-node application", or put `CONFIG_APP_xxx=...` into `prj.conf`:
`APP_CAMERA_NAME`, `APP_IMAGE_BUF_SIZE`, `APP_SESSION_TIMEOUT_S`, `APP_TRANSFER_TIMEOUT_S`,
`APP_IMAGE_WAKE_PULSE_MS`, `APP_BATTERY_WAKE_PULSE_MS`, `APP_PROGRESS_LOG_BYTES`, `APP_AUTO_REPAIR`. **Logs off** (final build): `CONFIG_LOG=n`.

## Porting DK -> XIAO nRF52840

Everything board specific is in `boards/`. Final steps:
1. Choose the real pins and edit `zephyr,user` (`trigger-gpios`, `cam-wake-gpios`) in `boards/xiao_ble.overlay`.
   The trigger polarity and pull follow the other MCU's signal (`GPIO_ACTIVE_HIGH`, no pull, ...).
2. The app is a single image there (no network core), `west build -b xiao_ble`.
3. RAM is 256 KB: keep `APP_IMAGE_BUF_SIZE` at about 128 KB or less.
4. The XIAO already has the LoRa radio defined; the picture in RAM is the place to hand to it
   (`get_image()`).

## Unverified assumptions

* Zephyr 4.4.2 API/Kconfig names were written from knowledge of 4.x, nothing was compiled.
  Likely trouble spots: `sysbuild.cmake` / `Kconfig.sysbuild` (copied from Zephyr's
  `peripheral_hr` sample pattern), missing west modules, a renamed Bluetooth Kconfig symbol.
  Fallback for the network core: build `$ZEPHYR_BASE/samples/bluetooth/hci_ipc` for
  `nrf5340dk/nrf5340/cpunet` yourself and flash both images.
* Camera node: after connecting it must be the one that starts pairing (it does, `ble_gap_security_initiate`).
* `bt_disable()` / `bt_enable()` again works on the nRF5340 (network core via IPC) and nRF52840; if
  `bt_disable` fails it is logged and Bluetooth just stays on.
* Pin P1.04 on the DK is free (verify), XIAO D0/D6 are placeholders.
* Throughput depends on MTU/data-length on both sides; first runs will show the real B/s in the log.
