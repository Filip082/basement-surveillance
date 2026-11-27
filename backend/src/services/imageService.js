const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { pipeline } = require('stream/promises');

const IMAGE_DIR = process.env.IMAGE_DIR || '/data/images';
const MAX_IMAGE_BYTES = Number(process.env.IMAGE_MAX_BYTES || 10485760);
const DOWNLOAD_TIMEOUT_MS = Number(
    process.env.IMAGE_DOWNLOAD_TIMEOUT_MS || 15000
);

async function downloadImage(imageUrl) {
    const controller = new AbortController();

    const timeout = setTimeout(
        () => controller.abort(),
        DOWNLOAD_TIMEOUT_MS
    );

    try {
        const response = await fetch(imageUrl, {
            signal: controller.signal,
        });

        if (!response.ok) {
            throw new Error(`Image download failed: HTTP ${response.status}`);
        }

        if (!response.body) {
            throw new Error('Image response has no body');
        }

        const contentType = response.headers.get('content-type') || '';

        if (!contentType.toLowerCase().startsWith('image/')) {
            throw new Error(`Invalid content type: ${contentType}`);
        }

        const contentLength = Number(response.headers.get('content-length'));

        if (Number.isFinite(contentLength) && contentLength > MAX_IMAGE_BYTES) {
            throw new Error('Image exceeds maximum allowed size');
        }

        await fs.promises.mkdir(IMAGE_DIR, { recursive: true });

        const filename = `${Date.now()}-${crypto.randomUUID()}.jpg`;
        const filePath = path.join(IMAGE_DIR, filename);

        let downloadedBytes = 0;

        const { Readable, Transform } = require('stream');

        const sizeLimiter = new Transform({
            transform(chunk, encoding, callback) {
                downloadedBytes += chunk.length;

                if (downloadedBytes > MAX_IMAGE_BYTES) {
                    callback(new Error('Image exceeds maximum allowed size'));
                    return;
                }

                callback(null, chunk);
            },
        });

        await pipeline(
            Readable.fromWeb(response.body),
            sizeLimiter,
            fs.createWriteStream(filePath, { flags: 'wx' }),
            { signal: controller.signal }
        );

        return {
            filePath,
            filename,
            size: downloadedBytes,
        };
    } catch (error) {
        throw new Error(`Image download error: ${error.message}`);
    } finally {
        clearTimeout(timeout);
    }
}

module.exports = { downloadImage };