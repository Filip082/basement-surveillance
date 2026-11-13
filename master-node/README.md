# Master-node LoRa sender

Reusable **C/Zephyr 4.4.2** communication component for the Seeed XIAO nRF52840 and Wio-SX1262 in [basement-surveillance](https://github.com/Filip082/basement-surveillance). A temporary demo sends one JSON test ENTRY per iteration. Entry detection, camera acquisition, telemetry collection and message queues are future application work.

```text
future sensors/camera -> sender API -> raw LoRa DATA + END -> Raspberry Pi border router
                             ^             ACK / NACK <---         |
                             |                                  SQLite -> HTTP backend
                     durable message ID
```

The authoritative wire contract is **version 1** in the sibling [border-router source and README](../border-router/README.md). This firmware uses raw LoRa, not LoRaWAN. An ACK confirms durable receipt at the router, independently of HTTP delivery. A failed session can still have reached the router if the final ACK was lost.

## Architecture and Zephyr APIs

- `protocol.c` is portable C11: CRC32, byte serialization, fragmentation and strict matching ACK/NACK validation. It has no Zephyr or radio dependency.
- `tx_session.c` is a blocking, bounded sender state machine using injected send/receive/time callbacks. It retains message metadata across retries and can later be called by an application-owned queue worker.
- `lora_transport.c` binds that state machine to Zephyr's `lora0` device, `lora_config()`, `lora_send()`, `lora_recv()` and `lora_airtime()`. It owns the half-duplex radio and enforces independent sender duty spacing with `k_msleep()`.
- `message_ids.c` uses Zephyr NVS and the board's existing flash storage partition to reserve monotonically increasing IDs before use.
- `main.c` is only a test application. Zephyr supplies the radio/SPI/GPIO drivers, DIO1 interrupt processing, sleeping/timing, mutex, flash storage and USB console. The application adds no heap allocations, radio thread or queue.

Only one session runs at a time. The public transport returns `-EBUSY` if another application thread already owns it. Call these blocking APIs from thread context, never from an ISR. The caller keeps the payload alive and unchanged until completion.

## Development environment

1. Install Docker and VS Code with the **Dev Containers** extension.
2. Open **`master-node/` itself**, rather than the whole repository, in VS Code.
3. Run **Dev Containers: Reopen in Container**.

The existing Dev Container is preserved. Its Debian image includes CMake, Ninja, DTC, gperf, Git, west, Python dependencies, **Zephyr SDK 1.0.1** and the ARM toolchain. `west.yml` pins Zephyr to **v4.4.2** and imports only `cmsis_6`, `hal_nordic` and `loramac-node`.

Inside the container:

```text
/workdir/                    persistent master-node-zephyr Docker volume
  .west/
  zephyr/                    pinned Zephyr source
  modules/                   pinned CMSIS, Nordic HAL and LoRaMac backend
  master-node/               bind mount of this directory
```

The first start initializes the workspace and runs `west update --narrow -o=--depth=1`; later starts reuse the volume. The host sees build output under `master-node/build/`. USB flashing and console access can be done from the host, without passing USB devices into Docker.

## Project structure and configuration

| File | Role |
|---|---|
| `include/protocol.h`, `src/protocol.c` | Protocol constants, validated message metadata, CRC32 and frame codecs |
| `include/tx_session.h`, `src/tx_session.c` | Portable DATA/END/feedback state machine, integer duty-spacing helper and attempt counters |
| `include/lora_transport.h`, `src/lora_transport.c` | Zephyr radio configuration, sync-word compatibility adapter, duty scheduling and application send API |
| `include/message_ids.h`, `src/message_ids.c` | Persistent message-ID block reservations in NVS |
| `src/main.c` | Optional repeated test ENTRY loop and delivery logging |
| `tests/test_sender.py` | Compiles the real C sender and ID allocator as host shared libraries; checks interoperability, bounded retries, timing and ID persistence |
| `tests/message_ids_fake.c`, `tests/nvs_fake/` | Host-only flash/NVS interface fake for the real allocator; not compiled into firmware |
| `Kconfig` | Application options, ranges and defaults |
| `prj.conf` | Enables LoRa, the pinned LoRaMac backend, logging and the test sender; sets a 4096-byte main stack |
| `boards/xiao_ble.overlay` | `lora0`, SPI/control pins, RF switching and TCXO; disables connector I2C because D4/D5 are used by the radio |
| `CMakeLists.txt` | Registers C sources/includes and preserves the `cdc-acm-console` snippet |
| `west.yml` | Pins Zephyr and the allowlisted backend/SoC modules |
| `.devcontainer/Dockerfile`, `devcontainer.json` | Shared build tools, west initialization and persistent workspace layout |

**New C files must be registered in `CMakeLists.txt` with `target_sources()`. Creating a file under `src/` does not automatically compile it.** For example, adding `lora_transport.c`, `protocol.c`, or a future `packet_queue.c` requires an entry such as:

```cmake
target_sources(app PRIVATE
  src/main.c
  src/protocol.c
  src/tx_session.c
  src/lora_transport.c
)
```

There is no packet queue in this implementation. `message_ids.c` is conditionally registered with `target_sources_ifdef(CONFIG_APP_PERSISTENT_IDS ...)`.

Use `prj.conf` for application choices, `west build -t menuconfig` to explore Kconfig, the overlay for physical hardware, and CMake for the source build graph. Do not move wiring into C constants.

| Application setting | Default | Meaning |
|---|---:|---|
| `CONFIG_APP_LORA_DEMO` | `y` | Enable the temporary test loop; set `n` to integrate real application logic |
| `CONFIG_APP_PERSISTENT_IDS` | `y` | Build the NVS message-ID allocator; required by the demo |
| `CONFIG_APP_DEVICE_ID` | `1` | Provision a different ID for each physical sender |
| `CONFIG_APP_DEMO_INTERVAL_SECONDS` | `60` | Sleep after a completed or failed session; radio duty delays apply in addition |
| `CONFIG_APP_LORA_FREQUENCY` | `868100000` | Hz; occupied channel must fit the 868.0–868.6 MHz profile |
| `CONFIG_APP_LORA_SF` | `7` | SF7–SF12; must match the router |
| `CONFIG_APP_LORA_BANDWIDTH` | `125000` | 125000, 250000 or 500000 Hz; must fit the configured sub-band |
| `CONFIG_APP_LORA_TX_POWER` | `14` | Nominal dBm; configurable downward to -9 |
| `CONFIG_APP_LORA_DUTY_PERMILLE` | `10` | Sender duty fraction: 10/1000 = 1%; can be made stricter |
| `CONFIG_APP_ROUTER_DUTY_PERMILLE` | `10` | Router fraction used for calculating the feedback window |
| `CONFIG_APP_ROUTER_QUIET_MS` | `350` | Router quiet period used for feedback timing |
| `CONFIG_APP_LORA_FEEDBACK_TIMEOUT_MS` | `20000` | Minimum RX window; increased automatically for slow modulation/router duty spacing |
| `CONFIG_APP_LORA_END_ATTEMPTS` | `8` | Maximum END/feedback rounds per message |
| `CONFIG_APP_LORA_PACKET_ATTEMPTS` | `2` | Maximum send attempts per packet after an I/O error |

For temporary settings, create an additional `.conf` fragment and build with `-DEXTRA_CONF_FILE=your.conf`. Changing a demo option does not alter the protocol or the companion router.

## Build and flash

Run inside the Dev Container, from `/workdir/master-node`:

```bash
west build -b xiao_ble                 # incremental
west build -b xiao_ble -p always       # clean build
west build -t menuconfig              # inspect/change configuration
```

To compile with the test loop disabled, create `demo-off.conf` containing `CONFIG_APP_LORA_DEMO=n`, then run:

```bash
west build -b xiao_ble -p always -- -DEXTRA_CONF_FILE=demo-off.conf
```

The reusable sender remains compiled. Replace the disabled `main` branch with application initialization and calls to the public API. After changing fragments, use a pristine build to avoid stale configuration.

Output: `build/zephyr/zephyr.uf2`, also visible on the host. To flash without special tools:

1. Connect the XIAO over USB-C.
2. Double-tap reset to enter its UF2 bootloader; a USB drive appears.
3. Copy `zephyr.uf2` onto that drive.
4. The drive disappears and the board reboots into the new firmware.

This uses the existing application partition and preserves the UF2/SoftDevice reserved layout. Normal application flashing should preserve NVS; a mass erase, changing partitions, corrupt storage or replacing the board can destroy ID history. Reprovision the device ID before reusing cleared storage against a router that still retains prior IDs.

### USB console

The `cdc-acm-console` snippet remains enabled in CMake, so logs use USB rather than D6/D7 UART pins. Open the serial port on the host:

- Windows: find the COM port in Device Manager, then use PuTTY at 115200 or `python -m serial.tools.miniterm COM6 115200`.
- Linux: `screen /dev/ttyACM0 115200` (quit with Ctrl+A, then K). For permission errors, add your account to `dialout` and log in again.

The demo waits two seconds for enumeration and does not wait indefinitely for DTR. Early messages can be missed before the terminal opens. Logs include device/message IDs, fragment count, duty delays, END rounds, ACK/NACK results, missing indices and final delivery status. Malformed/unrelated traffic is debug-level logging.

## Wio-SX1262 wiring

The existing overlay describes the **XIAO nRF52840 kit wiring**, not the different ESP32-S3 kit wiring. Verify the actual module revision before applying power. The Wio-SX1262 is a radio peripheral; the nRF52840 runs the application.

| Signal | XIAO pin | nRF52840 pin | Behavior |
|---|---|---|---|
| SCK | D8 | P1.13 | `xiao_spi` / SPI2 clock |
| MISO | D9 | P1.14 | SPI input |
| MOSI | D10 | P1.15 | SPI output |
| NSS | D4 | P0.04 | Active-low chip select |
| RESET | D2 | P0.28 | Active low |
| BUSY | D3 | P0.29 | Active high |
| DIO1 | D1 | P0.03 | TX/RX IRQ to Zephyr |
| RF_SW | D5 | P0.05 | RX-enable: high for RX, low otherwise |
| VCC/GND | 3.3 V/GND | — | 3.3 V power and logic |

The driver uses `rx-enable-gpios` for RF_SW, `dio2-tx-enable` for the internal TX switch, and DIO3 TCXO at **1.8 V** with **5 ms** startup. No application bit-banging is needed. The connector I2C bus shares D4/D5 and is disabled in this overlay. Attach a suitable 868 MHz antenna before transmitting.

## Radio interoperability

Defaults match the Python router's actual configuration:

| Setting | Both peers |
|---|---|
| Carrier | 868100000 Hz |
| Modulation | SF7, BW125 kHz, coding rate 4/5 |
| Preamble/header | 12 symbols, explicit header |
| PHY CRC / IQ | Enabled / normal |
| Raw SX1262 sync word | **0x0741** |
| Nominal TX power | 14 dBm |

The selected LoRaMac driver enables LDRO at SF11/SF12 with BW125 kHz and SF12 with BW250 kHz, matching the Python router's symbol-time rule. Change matching modulation on both peers when using those modes.

### Why the sync-word adapter exists

The pinned [Zephyr public LoRa API](https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.2/include/zephyr/drivers/lora.h) exposes `public_network`, not an arbitrary 16-bit sync word. In the [pinned LoRaMac module](https://github.com/zephyrproject-rtos/loramac-node/tree/fb00b383072518c918e2258b0916c996f2d4eebe), `false` writes **0x1424**, not **0x0741**. The latter is this project's custom raw value, despite the Python driver's private-word label.

`lora_transport.c` explicitly selects the existing LoRaMac backend. After each public `lora_config()` call, it uses that backend's existing `SX126xWriteRegisters()` and `SX126xReadRegisters()` exports to set/read back registers 0x0740–0x0741 as bytes `07 41`. The accessors perform the driver's normal wake/BUSY/SPI handling, and subsequent warm sleep retains the registers. There is one radio owner and no direct application SPI access. A failed readback fails the operation.

This small compatibility adapter is tied to **Zephyr 4.4.2 and the pinned module**, and must be rechecked before changing backends or upgrading. No router default, wire version or GPIO mapping was changed to hide the mismatch. Do not remove the adapter and assume `public_network=false` is sufficient.

The same backend configures a fixed 4000 ms software TX timeout. At SF7 the default DATA payload is 200 bytes. At slower modulation, initialization selects a smaller payload if needed to keep a DATA frame's calculated airtime at most 3750 ms. Variable fragment lengths are supported by protocol v1; both peers do not need the same chunk-size setting. The receiver's one-million-byte/10,000-fragment limits still apply.

## Whole-message protocol and retry bounds

The 22-byte header is exactly Python `!2sBBHIHHII`: `BS`, version 1, kind, device ID, message ID, fragment index/count, full body size and full-body CRC32. Integers are unsigned big endian. Kinds are ENTRY=1, IMAGE=2, TELEMETRY=3, ACK=4, NACK=5, END=6. ENTRY/TELEMETRY bodies should be UTF-8 JSON objects; IMAGE bodies are raw JPEG bytes. The sender API accepts binary buffers, including NUL bytes.

The sender transmits DATA, then END containing the original kind, then immediately listens. ACK must match every message field. NACK must contain 1–32 ascending unique in-range indices and matching metadata. Only those fragments are resent, followed by another END. Lost END or feedback causes **END-only retries**. Malformed/irrelevant packets do not extend the receive deadline. Repeated matching NACKs are safe, although they may consume another bounded retransmission round.

After at most `APP_LORA_END_ATTEMPTS` rounds, the call returns `-ETIMEDOUT`; this is not proof that the router failed to receive the body. TX I/O errors have a separate bounded per-packet retry count. Unrecovered receive errors return their negative errno. IDs/checksums remain stable within the session. A caller can retry the same immutable message with the same reserved ID after an ambiguous failure; a distinct application message always needs a fresh ID.

The router may ACK after a duty delay, and a previous large NACK can impose a longer off-time. The receive window is at least 20 seconds and is increased using the maximum NACK airtime, configured router duty fraction, quiet interval and a scheduling margin. It stays in RX during the entire logical wait, apart from short reconfiguration gaps after received packets. Do not use an arbitrary one-second deadline. Multiple nodes still need higher-level channel coordination; another sender continuously occupying the channel can cause bounded failure.

See [border-router/README.md](../border-router/README.md) for full field offsets, durable reassembly, NACK pagination, retention and HTTP behavior.

## Independent sender duty-cycle scheduling

The router's limiter cannot account for sender airtime. Before each DATA/END/retransmission, this firmware waits until its own next permitted TX time. Airtime comes from `lora_airtime()` under the actual TX configuration, rounded to milliseconds plus 5 ms margin. After an attempted transmission, the sender waits at least `airtime * (1 / duty_fraction - 1)` after completion, including failed attempts. It sleeps with Zephyr timing APIs and never busy-waits for a duty interval.

At boot it waits one full maximum-packet spacing before the first transmission. This conservative guard covers an immediately preceding transmission when elapsed off-time cannot be trusted across resets. It can take tens of seconds at SF7, and much longer at slow SF/stricter limits. It is separate from the demo's 60-second interval. Receiver feedback windows remain open while the router waits for its independent allowance.

The implemented profile requires the entire channel to fit **868.0–868.6 MHz** and allows at most 1% TX duty, configurable downward. Other sub-bands need a separately reviewed profile. TX power is not radiated ERP: check antenna gain, losses, hardware characteristics and national regulations. Firmware timing alone is not a compliance certification. There is no random backoff/channel allocation between multiple senders. Like protocol v1 on the router, the link is unencrypted and unauthenticated; CRC32 detects accidental corruption rather than proving sender identity.

### IDs and flash lifetime

The demo's device ID must be unique per physical node. NVS owns the board's existing 32 KiB `storage_partition` at 0xEC000, preserving bootloader/application boundaries. It saves the high-water sequence value in **blocks of 32 before issuing any ID from a block**. Power loss skips unused IDs; it does not intentionally reuse them. Retransmissions do not reserve another ID. This reduces flash writes compared with saving every message.

The allocator fails closed on detected storage errors or counter exhaustion; it never intentionally wraps to zero. Do not share this partition with a different filesystem or erase ID history without reprovisioning. Concurrent future callers must serialize calls to the allocator. The transport itself serializes sessions with a mutex.

## Automated and hardware verification

### Host compatibility tests

From `master-node/`, with Python 3.15 and a host C11 compiler (`cc`, or set `CC`):

```bash
python3.15 -m unittest discover -s tests -v
```

The Dev Container's system Python is a build tool and may be older than Python 3.15. Run these tests on a host with the router's Python 3.15 environment, for example from the repository root:

```bash
cd border-router
uv sync --locked
uv run --locked python -m unittest discover -s ../master-node/tests -v
```

The tests compile the actual `protocol.c` and `tx_session.c` with `-std=c11 -Wall -Wextra -Werror -pedantic`. C DATA/END frames are compared byte-for-byte with the real Python encoder and decoded by it. Python ACK/NACK frames are parsed by C. A simulated link runs the C state machine against the real Python SQLite store and feedback controller with packet/feedback loss, NACK pagination, invalid traffic, I/O errors and exhausted retries. Additional tests check integer duty-spacing rounding/overflow and the actual C ID allocator across simulated reboot, failed commits and exhausted/corrupt records. The NVS interface is faked; these tests do not verify Zephyr NVS itself or physical flash power-loss behavior. Tests do not claim radio, USB or flash-hardware verification.

### Verification performed for this implementation

- **19 host tests passed**, compiling the actual portable sender/allocator with strict C11 warnings and checking DATA/END/ACK/NACK against the real Python router.
- A pristine `west build -b xiao_ble -p always` passed in the unchanged Dev Container with Zephyr **4.4.2**, SDK **1.0.1** and ARM GCC **14.3.0**, producing `zephyr.uf2`.
- Separate demo-disabled and SF12 configurations also compiled. These are compile checks, not a simulated RF link.
- Physical USB console, flash retention under real power loss, RF output, TCXO startup and an over-the-air ACK/NACK exchange still require the devices.

### On-device check

1. Enable `CONFIG_APP_LORA_DEMO=y`, provision `CONFIG_APP_DEVICE_ID`, clean-build and flash.
2. Enable Pi SPI and start the existing router from the repository root: `docker compose --profile hardware up -d --build`.
3. Open the XIAO USB console and follow router logs: `docker compose logs -f border-router`.
4. Allow the startup duty guard to elapse. Expect a test ENTRY with a new message ID, END, then a matching ACK. Sender logs report `Delivered`; router logs report assembly and ACK transmission.
5. Reset the sender and confirm that its next ID advances past the previously reserved block.

The companion backend currently needs its ingestion endpoints implemented. The router can durably receive and ACK the test event while HTTP retries continue; that condition is distinct from an RF failure. No real radio exchange is claimed until this procedure has been performed on the actual devices.

## Future integration

Disable `CONFIG_APP_LORA_DEMO`, retain `CONFIG_APP_PERSISTENT_IDS`, and initialize the allocator/transport once from application thread context. For each new entry, telemetry object or JPEG body:

```c
uint32_t id;
struct bs_tx_result result;

/* Check every return value; reserve once for a new application message. */
int ret = bs_message_id_next(&id);
if (ret == 0) {
    ret = bs_lora_send_message(BS_IMAGE, my_device_id, id, jpeg_bytes, jpeg_size, &result);
}
```

Use `BS_ENTRY`/`BS_TELEMETRY` for JSON bytes and `BS_IMAGE` for JPEG bytes. The application does not encode fragments or process individual ACKs. A future queue can own immutable buffers, call this blocking API from one worker and decide when to retry/persist an ambiguously failed message; no such queue is implemented here.

## Troubleshooting and upgrading

- **west modules missing:** run `west update --narrow -o=--depth=1` from `/workdir`. Keep the allowlist and pinned commits consistent.
- **New function missing at link time:** register its C file with `target_sources()` and do a pristine build.
- **Unknown Kconfig option or stale generated headers:** rebuild with `-p always`; application `Kconfig` must source `Kconfig.zephyr`.
- **Radio not ready or BUSY/sync readback errors:** check module kit revision, 3.3 V/GND, NSS, reset, BUSY and the overlay. Persistent BUSY faults can require a power cycle; upstream driver BUSY handling is not an application watchdog.
- **No ACK:** verify raw sync word 0x0741 on both peers, channel/SF/BW/CR/CRC/IQ/preamble, RF_SW/DIO2, DIO1 IRQ wiring, TCXO configuration and the calculated receive window. Check that the router received END and has TX duty allowance. Antenna/coverage problems and another sender occupying the radio can also cause failures.
- **Long pauses before TX:** inspect the logged startup guard and duty delays; payloads at slower SF can take minutes or longer to transfer lawfully.
- **NVS initialization or ID reservation failure:** preserve storage for diagnosis. Do not reset the sequence as a workaround against an existing router identity.
- **USB console missing:** use a data-capable cable, leave bootloader mode, check the host serial device/group permissions and preserve `cdc-acm-console`.

To upgrade Zephyr, change `west.yml` and `.devcontainer/Dockerfile` together, keep SDK compatible with `zephyr/SDK_VERSION`, rebuild the container and update the west workspace. Re-audit sync-word exports, warm-sleep retention, TX timeout and LDRO behavior before changing backend or module revisions.

## References

- [Zephyr repository](https://github.com/zephyrproject-rtos/zephyr) and [pinned 4.4.2 LoRa API](https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.2/include/zephyr/drivers/lora.h): public device/configuration/send/receive contracts.
- [Zephyr LoRa samples](https://docs.zephyrproject.org/latest/samples/drivers/lora/index.html): API examples; use the pinned source when latest documentation differs.
- [Devicetree](https://docs.zephyrproject.org/latest/build/dts/index.html), [Kconfig](https://docs.zephyrproject.org/latest/build/kconfig/index.html), and [west](https://docs.zephyrproject.org/latest/develop/west/index.html): hardware description, build-time settings and workspace management.
- [Dev Containers specification](https://containers.dev/): shared development environment.
- [Seeed Wio-SX1262 documentation](https://wiki.seeedstudio.com/wio_sx1262/) and its module datasheet: RF switch, TCXO and electrical requirements.
- [Semtech SX126x reference driver](https://github.com/Lora-net/sx126x_driver): modem behavior and airtime reference.
- [LoRaRF-Python](https://github.com/chandrawi/LoRaRF-Python): companion Pi radio driver; not an nRF52840 dependency.
- [Basement-surveillance](https://github.com/Filip082/basement-surveillance) and [border-router/README.md](../border-router/README.md): project architecture and authoritative protocol v1 contract.
