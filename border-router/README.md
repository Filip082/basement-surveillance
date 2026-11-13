# Basement border router — version 1

Python **3.15** service connecting a Raspberry Pi 4B and Seeed Wio-SX1262 to an HTTP backend. It receives raw LoRa fragments, assembles and CRC32-checks complete messages in SQLite, sends whole-message ACK/NACK feedback, and retries HTTP delivery independently. This is a custom point-to-point protocol, not LoRaWAN.

This directory is the `border-router/` component of the larger [basement-surveillance project](../README.md). Run the container through the repository-root [`docker-compose.yml`](../docker-compose.yml), one directory above this README. The stack includes the backend, database and frontend; the router is enabled through the optional `hardware` profile.

Project version, wire protocol, SQLite schema and HTTP idempotency namespace all start at **1**. Sender firmware must use the version 1 format below. Existing databases with a different schema version are rejected: keep them for inspection and select a fresh database with `--db`.

## Hardware and references

- [LoRaRF-Python driver and source](https://github.com/chandrawi/LoRaRF-Python)
- [Wio-SX1262 for XIAO — Kamami product listing](https://kamami.pl/moduly-rozszerzajace-do-xiaoqt-py/1201747-wio-sx1262-modul-lora-868mhz-dla-xiao-5902186331051.html)
- [Seeed Wio-SX1262 documentation](https://wiki.seeedstudio.com/wio_sx1262/)
- [Seeed module datasheet and pin descriptions](https://files.seeedstudio.com/products/SenseCAP/Wio_SX1262/Wio-SX1262-N_Module_Datasheet.pdf)

Use 64-bit Raspberry Pi OS and a suitable 868 MHz antenna. Docker supplies Python 3.15 and the radio dependencies inside the container; host Python and uv are needed only for the optional direct installation below. The Wio board is an SPI radio peripheral; it needs a host processor. Its XIAO connectors require an adapter or individual wires to a Pi. Use the board's signal labels and schematic to identify connections.

### Wiring

The default connection uses SPI0 CE0. GPIO labels below use **BCM numbering**, not physical header positions.

| Wio signal | Raspberry Pi signal | Physical header pin |
|---|---|---:|
| VCC | 3.3 V | 1 |
| GND | Ground | 6 |
| SCK | GPIO11 / SPI0 SCLK | 23 |
| MOSI | GPIO10 / SPI0 MOSI | 19 |
| MISO | GPIO9 / SPI0 MISO | 21 |
| NSS | GPIO8 / SPI0 CE0 | 24 |
| NRST | GPIO22 | 15 |
| BUSY | GPIO23 | 16 |
| RF_SW | GPIO24 | 18 |

RF_SW must be high during reception and low during transmission. DIO2 controls the module's internal TX switch; DIO3 supplies the TCXO, configured here at 1.8 V. DIO1 can remain disconnected because the service polls IRQ status over SPI. The driver adapters retain RESET and RF_SW ownership and leave NSS to hardware SPI. Connect the antenna before transmitting and use 3.3 V logic.

### Enable SPI on the Raspberry Pi

Run `sudo raspi-config`, select **Interface Options → SPI → Enable**, then reboot. Alternatively, add this setting under an applicable section such as `[all]` in `/boot/firmware/config.txt` (`/boot/config.txt` on older images):

```ini
dtparam=spi=on
```

After reboot:

```bash
ls -l /dev/spidev0.0 /dev/gpiochip*
gpioinfo
sudo usermod -aG spi,gpio "$USER"
```

Install the `gpiod` command-line package if `gpioinfo` is missing. Log out and back in after changing groups. Inspect the chip labels and line offsets; `--gpio-chip 0` assumes the Pi's BCM lines are on `/dev/gpiochip0`. Other OS images may number chips differently. The Python `gpiod` binding installed by uv is separate from the OS command-line utility. See [Raspberry Pi configuration documentation](https://www.raspberrypi.com/documentation/computers/configuration.html).

## Run through the project's Docker Compose stack

Run these commands on the Raspberry Pi with the radio connected and SPI enabled. From this `border-router/` directory, move up to the **repository root**, containing `docker-compose.yml`, `.env.example`, `backend/` and `border-router/`. All subsequent Compose commands run from that root:

```bash
cd ..

# First setup only: create .env if it does not exist, then edit its settings.
test -f .env || cp .env.example .env

docker compose --profile hardware config --quiet
docker compose --profile hardware up -d --build
docker compose logs -f border-router
```

This starts the frontend, backend, database and router. To start just the router and its backend/database dependencies, use `docker compose --profile hardware up -d --build border-router`. Ordinary `docker compose up -d` omits the router because its `hardware` profile is optional.

The router service in the parent Compose file provides:

| Configuration | Value and purpose |
|---|---|
| Build context | `./border-router`, using this directory's `Dockerfile` |
| Startup | Image entrypoint `bs-border-router`, with Python 3.15 and the locked radio dependencies |
| Backend URL | `http://backend:${PORT:-8000}` on the shared Compose network |
| Radio devices | `/dev/spidev0.0` for SPI0 CE0 and `/dev/gpiochip0` for GPIO line requests |
| SQLite path | `/data/router-v1.sqlite3`, configured through `BORDER_DB` |
| Persistent storage | Named volume `border-router-data` mounted at `/data` |
| Dependencies | Starts `backend`, which waits for the database health check |
| Restart behavior | `unless-stopped` |

The backend does not need a published host port for the router to reach it. Container DNS resolves `backend`; `127.0.0.1` inside the router would address the router itself. `depends_on` orders startup but does not wait for the HTTP API to become ready; persisted HTTP retries handle that interval. The backend still needs the ingestion endpoints described below before delivery can succeed.

The GPIO mapping matches the default `GPIO_CHIP=0`. If your Pi uses another chip, update both the `devices` mapping and the router's `GPIO_CHIP` environment setting. `/dev/gpiomem` cannot replace `/dev/gpiochipN` for this driver. The Compose service uses Docker's default root container user and mapped devices; it does not need privileged mode for the configured SPI/GPIO access.

### Change router settings in Compose

The root `.env` supplies Compose interpolation such as `PORT`. Radio variables in the configuration table below must also be passed into the router container: adding `LORA_SF` or `LORA_DUTY` to `.env` alone does not forward them through the current service definition. Add settings to the service's `environment` mapping, or create a repository-root `docker-compose.override.yml`, for example:

```yaml
services:
  border-router:
    environment:
      LORA_FREQ: "868100000"
      LORA_SF: "7"
      LORA_BW: "125000"
      LORA_DUTY: "0.01"
```

Apply configuration or source changes from the repository root with:

```bash
docker compose --profile hardware up -d --build border-router
```

The router source is copied into its image, so Python changes require rebuilding. `docker compose --profile hardware down` removes the stack's containers while retaining its volumes. Adding `-v` also deletes the router's queued messages, deduplication state and transmit-airtime history along with the other project volumes.

## Optional direct installation with uv

For development or running the router directly on the Pi, install [uv](https://docs.astral.sh/uv/getting-started/installation/), then run from the repository root:

```bash
cd border-router
sudo apt update
sudo apt install -y git build-essential
uv python install 3.15
uv sync --locked --extra radio
uv run --locked --extra radio bs-border-router \
  --backend-url http://127.0.0.1:8000 \
  --gpio-chip 0 --reset 22 --busy 23 --rf-switch 24
```

`.python-version` selects Python 3.15 and `pyproject.toml` requires the 3.15 series. `uv.lock` pins the resolved dependencies; keep it committed. Build tools cover systems without matching native `spidev` or `gpiod` wheels.

This direct-run example expects a backend listening on the Pi's host loopback port 8000. The project's default Compose configuration exposes the backend only inside its container network; use the Compose deployment above to connect to that backend without publishing another port. The standalone development receiver below is available for host-side tests.

### GitHub or a package install for LoRaRF?

**Use uv to install the pinned GitHub source through this project's `radio` extra.** GitHub is the source; uv is the installer. No manual clone or `setup.py` invocation is needed.

The published [LoRaRF 1.4.0 package](https://pypi.org/project/LoRaRF/1.4.0/) uses `RPi.GPIO` and the older constructor API. This router uses the `LoRaSpi` / `LoRaGpio` constructor API from [commit `39c664476dc447f50b8f5c1f1c9d77f3fea21140`](https://github.com/chandrawi/LoRaRF-Python/tree/39c664476dc447f50b8f5c1f1c9d77f3fea21140), which uses `gpiod` 2.x. Therefore a plain `uv add LoRaRF` would select an incompatible release. The pinned source still declares `RPi.GPIO` in its metadata, so `[tool.uv].exclude-dependencies` removes that stale dependency and the radio extra supplies `gpiod` explicitly. Use the documented uv workflow so this override is applied.

## Configuration and European 868 MHz limits

Both peers must use the same frequency, modulation, sync word, framing and low-data-rate optimization (LDRO). LDRO is enabled when symbol duration is at least 16 ms, including SF11/SF12 at BW125 kHz. Airtime accounting includes the distinct SF5/SF6 overhead described by the [Semtech SX126x reference driver](https://github.com/Lora-net/sx126x_driver/blob/master/src/sx126x.c).

| Setting | CLI option | Environment variable | Default |
|---|---|---|---|
| SQLite path | `--db` | `BORDER_DB` | `./router-v1.sqlite3` |
| Backend origin | `--backend-url` | `BACKEND_URL` | `http://127.0.0.1:8000` |
| GPIO chip | `--gpio-chip` | `GPIO_CHIP` | `0` |
| RESET / BUSY / RF_SW | `--reset` / `--busy` / `--rf-switch` | `RESET_GPIO` / `BUSY_GPIO` / `RF_SWITCH_GPIO` | `22` / `23` / `24` |
| Frequency, Hz | `--freq` | `LORA_FREQ` | `868100000` |
| Spreading factor | `--sf` | `LORA_SF` | `7` |
| Bandwidth, Hz | `--bw` | `LORA_BW` | `125000` |
| Coding-rate denominator | `--cr` | `LORA_CR` | `5` (4/5) |
| SX1262 sync word | `--sync-word` | `LORA_SYNC_WORD` | `0x0741` |
| Nominal TX power, dBm | `--tx-power` | `LORA_TX_POWER` | `14` |
| TX duty-cycle fraction | `--duty-cycle` | `LORA_DUTY` | `0.01` (1%) |
| RX quiet period, seconds | `--quiet-seconds` | — | `0.35` |

Other defaults: explicit LoRa headers, PHY CRC enabled, 12-symbol preamble and normal IQ. Supported bandwidths are 125/250/500 kHz; coding rates are 4/5 through 4/8. The pinned driver offers nominal SX1262 PA presets of 14, 17, 20 and 22 dBm. Its PA configuration and TX register setting work as a pair; do not overwrite only the TX register. Hardware capability does not imply permission to transmit at those powers.

For the default **868.0–868.6 MHz** sub-band, [ERC Recommendation 70-03](https://docdb.cept.org/document/845) describes a 25 mW ERP limit and a duty-cycle option of at most 1%, with alternative access techniques subject to their own requirements. National implementation still applies. The 1% setting is not appropriate for every European 868 MHz sub-band; for example, 868.7–869.2 MHz has a 0.1% duty-cycle option. Carrier bandwidth must fit the chosen sub-band. A nominal 14 dBm transmitter setting alone does not determine ERP: include antenna gain and losses.

The router retains the conservative **1% TX limiter by default**. It reserves estimated airtime plus a small ramp/timing margin in SQLite **before** each ACK/NACK attempt, counts failed attempts, enforces a rolling one-hour budget and waits between transmissions. Restarting with the same database retains this accounting. This guards this router's feedback only; the sender needs its own airtime accounting. The implementation does not enforce ERP, national rules or a frequency-dependent regulatory profile.

To **disable the implemented airtime restrictions completely**, use `--duty-cycle 0` or `LORA_DUTY=0` (pass `LORA_DUTY: "0"` into the router's Compose `environment` when running in Docker). The explanatory comments are in `FeedbackController.reserve_transmit()` in [src/feedback.py](src/feedback.py). Zero bypasses both the rolling budget and post-TX off-time, including existing saved reservations. It leaves the protocol's half-duplex quiet period in place. Use this only where unrestricted operation is permitted, such as an appropriate conducted or shielded test setup. Setting the fraction to `1` still performs accounting; zero is the complete bypass.

## Message delivery contract

1. The sender transmits DATA fragments followed by **one END** for the entire message.
2. SQLite commits every new fragment. Once all fragments arrive, the router checks body length and CRC32 and commits the complete body to its HTTP outbox.
3. **Only END queues feedback.** DATA and radio silence never independently trigger ACK/NACK.
4. After the RX quiet period and airtime allowance, the router sends ACK for a complete persisted message, or NACK containing up to 32 missing indices.
5. On NACK, the sender retransmits the listed DATA and sends END again. Repeat for further batches of missing fragments.
6. If END or feedback is lost, the sender retries END with bounded attempts and an airtime-aware timeout. Duplicate END produces another response.

```text
sender                              border router
  DATA #0 --------------------------> persist
  DATA #1 -- lost
  DATA #2 --------------------------> persist
  END ------------------------------>
       <----------------------------- NACK [1]
  DATA #1 --------------------------> assemble + commit complete body
  END ------------------------------>
       <----------------------------- ACK
                                      HTTP outbox retries independently
```

ACK means local durable receipt, independent of backend availability. HTTP forwarding may begin as soon as the last DATA completes the body, even before END. If full-body length/CRC fails, stored fragments for that message are discarded; the next END requests fragments from the beginning using the same message ID. If every DATA was lost, END still creates a NACK starting at index zero.

The sender must stop transmitting while waiting for feedback. A fixed one-second timeout is too short for many combinations of LoRa airtime, quiet time and duty-cycle off-time. Nodes must coordinate access to this single half-duplex radio.

## Wire protocol 1

All numbers are unsigned big endian. Each radio frame has a **22-byte header**, defined by `struct.Struct("!2sBBHIHHII")`:

| Offset | Bytes | Field | Meaning |
|---|---:|---|---|
| 0 | 2 | `magic` | ASCII `BS` |
| 2 | 1 | `version` | `1` |
| 3 | 1 | `kind` | `1` ENTRY, `2` IMAGE, `3` TELEMETRY, `4` ACK, `5` NACK, `6` END |
| 4 | 2 | `device_id` | Sender identifier |
| 6 | 4 | `message_id` | Identifier unique per device and DATA kind within retention |
| 10 | 2 | `part_index` | Zero-based DATA index; zero for control frames |
| 12 | 2 | `part_count` | Total DATA fragments |
| 14 | 4 | `total_size` | Full application body size, bytes |
| 18 | 4 | `crc32` | Full-body CRC32/ISO-HDLC, equivalent to `zlib.crc32(body)` |

Frame bodies:

- DATA: 1–233 bytes; 200 bytes per fragment is the reference default. ENTRY/TELEMETRY reassemble into UTF-8 JSON objects. IMAGE reassembles into raw JPEG bytes.
- END/ACK: one byte containing the original DATA kind.
- NACK: original DATA kind, then 1–32 ascending, unique `uint16` missing indices.

All frames repeat the same `(device_id, original_kind, message_id, part_count, total_size, crc32)` metadata. Conflicting metadata and conflicting duplicate fragments are rejected. Default receiver limits are **1,000,000 body bytes** and **10,000 fragments** per message; the binary fields can represent larger values, which this receiver rejects.

Reference encoder: `src.protocol.encode_message()`. Build END with `encode_control(Kind.END, decode_frame(data_frames[0]))`. Sender firmware must implement this layout; no firmware is generated by this package.

## HTTP backend and persistence

The backend must implement:

| Endpoint | Request body |
|---|---|
| `POST /api/events` | JSON reception metadata plus `payload`, an entry JSON object |
| `POST /api/telemetry` | JSON reception metadata plus `payload`, a telemetry JSON object |
| `POST /api/images` | Multipart `file` containing JPEG bytes plus scalar metadata fields |

Metadata fields are `device_id`, `message_id`, `received_at` (UTC ISO 8601), `rssi` (dBm) and `snr` (dB). JSON metadata uses `null` for unavailable measurements; multipart omits them. Every request includes `Idempotency-Key: bs-v1:<device_id>:<kind>:<message_id>`.

The backend must deduplicate this key: a successful response can be lost before the router records delivery. HTTP failures retain the body and defer the next attempt for five seconds, with a once-per-second worker poll. Retry deadlines survive restarts, and deferred messages allow newer messages to proceed. Invalid JSON objects remain queued and are logged for diagnosis. Monitor logs and database growth.

Partial assemblies expire after two days without progress. Sent records retain deduplication metadata for seven days; undelivered complete bodies are retained. Do not reuse message IDs during that window. Keep SQLite on persistent local storage with enough space; deleting the database also deletes pending messages, deduplication state and airtime accounting.

`--backend-url` accepts an HTTP(S) origin, including a Docker hostname such as `http://backend:8000`. `127.0.0.1` addresses the router's own network namespace. The companion `backend/src/index.js` currently exposes only a health endpoint, and `master-node/src/main.c` is a Hello World stub. Their HTTP ingestion, deduplication and sender protocol still need implementation; this router and its development receiver can be tested independently.

## Run without radio hardware

Run the following commands from `border-router/`. The simulation and unit tests need no radio extra:

```bash
uv sync --locked
uv run --locked python -m unittest discover -s tests -v
```

Start a development receiver in terminal A:

```bash
uv run --locked python dev_receiver.py --port 8765
```

In terminal B:

```bash
uv run --locked python -m src.demo | uv run --locked bs-border-router \
  --simulate-hex --backend-url http://127.0.0.1:8765 \
  --db ./router-v1-demo.sqlite3
```

The demo sends an entry, illustrative JPEG-like bytes and telemetry. It prints ACKs and saves three HTTP request bodies in `dev_received/`. The image is a framing fixture, not a decodable photograph. Simulation prints feedback immediately after each input line and bypasses radio duty-cycle accounting; use it to check protocol and HTTP behavior.

## Operational limits

CRC32 detects accidental corruption; it does not authenticate senders or encrypt images and events. This service has no key exchange or replay protection beyond retained message IDs. JPEG transfer can be slow under duty-cycle limits. Monitor storage, failed HTTP requests and sender retransmission limits. Validate wiring, measured RF output and end-to-end sender compatibility on the actual Pi before deployment.
