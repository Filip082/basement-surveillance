#!/usr/bin/env bash
# Build the firmware in the official ESP-IDF image (no VS Code / dev container needed).
# Usage: scripts/docker-build.sh [idf.py args...]     default: build
#   scripts/docker-build.sh menuconfig      (interactive, uses -it)
#   scripts/docker-build.sh fullclean
set -euo pipefail

IMAGE="${IDF_IMAGE:-espressif/idf:v6.1}"
cd "$(dirname "$0")/.."

ARGS=("${@:-build}")
TTY=()
[ -t 0 ] && [ -t 1 ] && TTY=(-it)

# Run as the host user so build/ and managed_components/ are not owned by root.
# HOME points at a writable dir for the component-manager cache.
exec docker run --rm "${TTY[@]}" \
  -u "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  -v "$PWD:/project" -w /project \
  "$IMAGE" idf.py "${ARGS[@]}"
