# Slim Zephyr build environment: only the ARM toolchain (XIAO nRF52840).
# The official ghcr.io/zephyrproject-rtos/zephyr-build image works too, but it is
# ~8 GB because it ships toolchains for every architecture.
FROM debian:trixie-slim

# Keep in sync with west.yml (Zephyr) and zephyr/SDK_VERSION (SDK)
ARG ZEPHYR_VERSION=v4.4.2
ARG ZEPHYR_SDK_VERSION=1.0.1
ARG USERNAME=dev

RUN apt-get update && apt-get install -y --no-install-recommends \
        git cmake ninja-build gperf ccache device-tree-compiler \
        wget xz-utils file make ca-certificates \
        python3 python3-venv \
    && rm -rf /var/lib/apt/lists/*

# Zephyr SDK: minimal bundle + arm-zephyr-eabi toolchain only
RUN wget -q https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZEPHYR_SDK_VERSION}/zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-x86_64_minimal.tar.xz \
    && tar -xf zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-x86_64_minimal.tar.xz -C /opt \
    && rm zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-x86_64_minimal.tar.xz \
    && /opt/zephyr-sdk-${ZEPHYR_SDK_VERSION}/setup.sh -t arm-zephyr-eabi
ENV ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-${ZEPHYR_SDK_VERSION}

# west + Zephyr's Python dependencies in a venv
RUN python3 -m venv /opt/venv \
    && /opt/venv/bin/pip install --no-cache-dir west \
    && wget -qO /tmp/req.txt https://raw.githubusercontent.com/zephyrproject-rtos/zephyr/${ZEPHYR_VERSION}/scripts/requirements-base.txt \
    && /opt/venv/bin/pip install --no-cache-dir -r /tmp/req.txt \
    && rm /tmp/req.txt
ENV PATH=/opt/venv/bin:$PATH

# Non-root user (UID 1000 so files on the host aren't owned by root).
# /workdir is the west workspace: Zephyr + modules live in a named volume there,
# this repo's master-node/ is bind-mounted into it.
RUN useradd -m -u 1000 -s /bin/bash -G dialout ${USERNAME} \
    && mkdir /workdir && chown ${USERNAME}:${USERNAME} /workdir
USER ${USERNAME}
WORKDIR /workdir
