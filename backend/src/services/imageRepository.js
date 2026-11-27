const pool = require('../config/database');

async function saveImageMetadata(filePath, eventTimestamp) {
    const query = `
        INSERT INTO images (file_path, event_timestamp)
        VALUES ($1, $2)
        RETURNING id, file_path, event_timestamp, created_at
    `;

    const values = [filePath, eventTimestamp];

    const result = await pool.query(query, values);

    return result.rows[0];
}

async function getAllImages(from = null, to = null) {
    let query = `
        SELECT id, file_path, event_timestamp, created_at
        FROM images
    `;

    const conditions = [];
    const values = [];

    if (from) {
        values.push(from);
        conditions.push(`event_timestamp >= $${values.length}::timestamptz`);
    }

    if (to) {
        values.push(to);
        conditions.push(`event_timestamp <= $${values.length}::timestamptz`);
    }

    if (conditions.length > 0) {
        query += ` WHERE ${conditions.join(' AND ')}`;
    }

    query += ` ORDER BY event_timestamp DESC`;

    const result = await pool.query(query, values);

    return result.rows;
}

async function getImageById(id) {
    const query = `
        SELECT id, file_path, event_timestamp, created_at
        FROM images
        WHERE id = $1
    `;

    const result = await pool.query(query, [id]);

    return result.rows[0] || null;
}

async function deleteImageById(id) {
    const query = `
        DELETE FROM images
        WHERE id = $1
        RETURNING id, file_path
    `;

    const result = await pool.query(query, [id]);

    return result.rows[0] || null;
}

async function getOldestImage() {
    const query = `
        SELECT id, file_path, event_timestamp, created_at
        FROM images
        ORDER BY event_timestamp ASC, created_at ASC, id ASC
        LIMIT 1
    `;

    const result = await pool.query(query);

    return result.rows[0] || null;
}

module.exports = {
    saveImageMetadata,
    getAllImages,
    getImageById,
    deleteImageById,
    getOldestImage,
};