# basement-surveillance

Group project for Data Acquisition Systems classes.

A battery-powered camera in a basement lab takes a photo of every person entering
the room. The photo travels over LoRa radio to a receiver next to a server, which
stores the events and serves a web app for browsing them.

## How it works

```
 Basement lab (battery, no network)               Server (Docker)
┌──────────────────────────────────┐     ┌──────────────────────────────────────────┐
│ camera-node ──► master-node ─────┼LoRa─┼─► border-router ──► backend ◄──► db      │
│ ESP32-S3 cam     nRF52840+SX1262 │     │   Python (planned)  Node/Express Postgres│
└──────────────────────────────────┘     │                       ▲                  │
                                         │                  frontend (React) ◄─ browser
                                         └──────────────────────────────────────────┘
```

The link between camera-node and master-node is not decided yet.

## Repository layout

| Dir              | What                                                     | Tech                         |
|------------------|----------------------------------------------------------|------------------------------|
| `camera-node/`   | Takes the photo                                          | DFRobot DFR1154, ESP-IDF 6.1 |
| `master-node/`   | Sends data over LoRa, handles sensors                    | XIAO nRF52840 + Wio-SX1262, Zephyr 4.4 |
| `border-router/` | Receives LoRa, forwards events to the backend            | Python                       |
| `backend/`       | REST API, stores events and images                       | Node 24, Express 5           |
| `frontend/`      | Web app for browsing events                              | React, Vite                  |

Each firmware folder has its own README with build and flash steps.

## Server: getting started

### Prerequisites

- Git
- Docker with Compose v2 (`docker compose version` should print v2 or later).
  Windows: Docker Desktop with the WSL 2 backend.

### First run

```bash
git clone <repo-url> basement-surveillance
cd basement-surveillance
cp .env.example .env        # Windows PowerShell: Copy-Item .env.example .env
docker compose up -d
```

Open <http://localhost:5173>. The first start takes a minute or two while `npm install`
runs inside the containers. Check that the API answers: <http://localhost:5173/api/health>
should return `{"status":"ok"}`.

### The `.env` file

All configuration lives in `.env`. Every service in `docker-compose.yml` loads it.

- `.env.example` is committed and holds safe defaults. Copy it to `.env`.
- `.env` is gitignored. Put local changes and real passwords there, never in git.
- New variable? Add it to `.env.example` too, so teammates get it.
- After editing `.env`, apply it with `docker compose up -d` (recreates changed services).

| Variable                        | Used by  | Meaning                                                  |
|---------------------------------|----------|----------------------------------------------------------|
| `POSTGRES_DB/USER/PASSWORD`     | db, backend | Database name and credentials                          |
| `PORT`                          | backend  | Port the API listens on inside the compose network       |
| `DATABASE_URL`                  | backend  | Postgres connection string (built from the values above) |
| `IMAGE_DIR`                     | backend  | Where photos are stored (the `images` volume)            |
| `FRONTEND_PORT`                 | frontend | Vite dev server port, also published on your machine     |
| `API_PROXY_TARGET`              | frontend | Where Vite forwards `/api` requests                      |

If port 5173 is already taken on your machine, change `FRONTEND_PORT` in `.env`.

### How the containers are set up

- Development setup: `backend/` and `frontend/` are mounted into the containers.
  Saving a file reloads it (nodemon restarts the backend, Vite hot-reloads the frontend).
  No image rebuilds needed.
- Only the frontend is reachable from your machine. The backend and database talk over
  the internal compose network; the frontend forwards `/api/*` to the backend.
- Data lives in Docker volumes: `db-data` (Postgres) and `images` (photos).
  It survives `docker compose down`, but not `docker compose down -v`.

### Everyday commands

| Task                                    | Command                                               |
|-----------------------------------------|-------------------------------------------------------|
| Start everything                        | `docker compose up -d`                                |
| See what's running                      | `docker compose ps`                                   |
| Follow logs (all / one service)         | `docker compose logs -f` / `docker compose logs -f backend` |
| Restart a service                       | `docker compose restart backend`                      |
| Add an npm package                      | `docker compose exec backend npm install <package>`   |
| Shell inside a container                | `docker compose exec backend sh`                      |
| Open the database                       | `docker compose exec db psql -U surveillance surveillance` |
| Stop everything (keeps data)            | `docker compose down`                                 |
| Stop and **delete all data**            | `docker compose down -v`                              |

Use `frontend` instead of `backend` in those commands for the web app.
Install npm packages through the container (not on your machine), so
`package-lock.json` matches the Node version everyone uses.

## Firmware

Both firmware projects build inside VS Code dev containers, so nobody needs to install
ESP-IDF or Zephyr locally. You need Docker and VS Code with the **Dev Containers** extension.

- [`camera-node/README.md`](camera-node/README.md): build in the container, flash with `esptool`
- [`master-node/README.md`](master-node/README.md): build in the container, flash by copying a `.uf2` file

Open the firmware folder itself in VS Code (not the repo root), then
**Dev Containers: Reopen in Container**.

## CI

GitHub Actions (`.github/workflows/ci.yml`) runs on every push to `main` and every pull request:

| Job           | Checks                                                  |
|---------------|---------------------------------------------------------|
| `backend`     | Installs dependencies, syntax-checks the server         |
| `frontend`    | Lint + production build                                 |
| `camera-node` | ESP-IDF firmware build                                  |
| `master-node` | Zephyr firmware build                                   |
| `docker`      | Validates `docker-compose.yml`, builds production images |

## Workflow

- Branch from `main`: `feature/<area>-<short-name>` (e.g. `feature/master-node-lora`)
- Open a pull request; CI must pass; one teammate reviews
