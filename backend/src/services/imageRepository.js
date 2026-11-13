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

async function getAllImages() {
    const query = `
        SELECT id, file_path, event_timestamp, created_at
        FROM images
        ORDER BY event_timestamp DESC
    `;

    const result = await pool.query(query);

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

module.exports = {
    saveImageMetadata,
    getAllImages,
    getImageById,
};