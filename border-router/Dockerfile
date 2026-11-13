FROM ghcr.io/astral-sh/uv:0.11.12 AS uv
FROM python:3.15-slim-bookworm AS build

COPY --from=uv /uv /uvx /bin/
WORKDIR /app

ENV UV_PYTHON_DOWNLOADS=never \
    UV_NO_DEV=1 \
    PYTHONUNBUFFERED=1 \
    BORDER_DB=/data/router-v1.sqlite3 \
    PATH="/app/.venv/bin:$PATH"

# Git installs the pinned driver; build tools cover missing ARM/Python wheels.
RUN apt-get update \
    && apt-get install -y --no-install-recommends git build-essential ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY pyproject.toml uv.lock README.md ./
RUN uv sync --locked --extra radio --no-install-project

COPY src ./src
RUN uv sync --locked --extra radio --no-editable

# Keep compilers and Git in the build stage; the installed wheel contains src.
FROM python:3.15-slim-bookworm
WORKDIR /app
ENV PYTHONUNBUFFERED=1 \
    BORDER_DB=/data/router-v1.sqlite3 \
    PATH="/app/.venv/bin:$PATH"
COPY --from=build /app/.venv /app/.venv
RUN mkdir -p /data

ENTRYPOINT ["bs-border-router"]
