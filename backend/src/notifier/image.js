const sharp = require('sharp');

// Turns a camera frame (Buffer or file path) into a small base64 JPEG for alerts.
async function toJpegBase64(input, { maxSide = 400, quality = 70 } = {}) {
    const jpeg = await sharp(input)
        .rotate()
        .resize({
            width: maxSide,
            height: maxSide,
            fit: 'inside',
            withoutEnlargement: true,
        })
        .jpeg({ quality })
        .toBuffer();

    return jpeg.toString('base64');
}

module.exports = { toJpegBase64 };
