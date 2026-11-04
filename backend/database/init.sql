CREATE TABLE IF NOT EXISTS images (
    id BIGSERIAL PRIMARY KEY,
    file_path TEXT NOT NULL,
    event_timestamp TIMESTAMPTZ NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_images_event_timestamp
ON images(event_timestamp);