const express = require('express');
const fs = require('fs');
const path = require('path');
const { downloadImage } = require('../services/imageService');
const { getImageById, saveImageMetadata, getAllImages } = require('../services/imageRepository');
const { enforceStorageLimit } = require('../services/storageService');
const { sendAlertInBackground } = require('../notifier');
const { photoAlert } = require('../notifier/alerts');

const router = express.Router();

router.post('/', async (req, res) => {
    const eventTimestamp = new Date();

    try {
        const imageUrl = process.env.ROUTER_IMAGE_URL;

        const result = await downloadImage(imageUrl);

        await enforceStorageLimit(result.size, result.filePath);

        const image = await saveImageMetadata(
            result.filePath,
            eventTimestamp
        );

        sendAlertInBackground(photoAlert(eventTimestamp, result.filePath));

        res.status(200).json({
            status: 'success',
            image,
            filename: result.filename,
            size: result.size,
        });

    } catch (error) {
        console.error('Image processing failed:', error);

        res.status(500).json({
            status: 'error',
            message: error.message,
        });
    }
});

router.get('/', async (req, res) => {
    try {
        const { from, to } = req.query;

        if (from && isNaN(Date.parse(from))) {
            return res.status(400).json({
                status: 'error',
                message: 'Invalid start date',
            });
        }

        if (to && isNaN(Date.parse(to))) {
            return res.status(400).json({
                status: 'error',
                message: 'Invalid end date',
            });
        }

        if (from && to && new Date(from) > new Date(to)) {
            return res.status(400).json({
                status: 'error',
                message: 'Start date must not be later than end date',
            });
        }

        const images = await getAllImages(from || null, to || null);

        res.status(200).json({
            status: 'success',
            images,
        });

    } catch (error) {
        console.error('Failed to fetch images:', error);

        res.status(500).json({
            status: 'error',
            message: 'Failed to fetch images',
        });
    }
});

router.get('/storage', async (req, res) => {
    try {
        const imageDir = process.env.IMAGE_DIR || '/data/images';

        const getDirectorySize = (dir) => {
            let totalSize = 0;

            const entries = fs.readdirSync(dir, { withFileTypes: true });

            for (const entry of entries) {
                const fullPath = path.join(dir, entry.name);

                if (entry.isDirectory()) {
                    totalSize += getDirectorySize(fullPath);
                } else if (entry.isFile()) {
                    totalSize += fs.statSync(fullPath).size;
                }
            }

            return totalSize;
        };

        const sizeBytes = getDirectorySize(imageDir);

        res.status(200).json({
            status: 'success',
            sizeBytes,
            sizeMB: Number((sizeBytes / (1024 * 1024)).toFixed(2)),
            sizeGB: Number((sizeBytes / (1024 * 1024 * 1024)).toFixed(3))
        });

    } catch (error) {
        console.error('Failed to calculate image storage:', error);

        res.status(500).json({
            status: 'error',
            message: 'Failed to calculate image storage'
        });
    }
});

router.get('/:id/file', async (req, res) => {
    try {
        const id = req.params.id;

        if (!/^\d+$/.test(id)) {
            return res.status(400).json({
                status: 'error',
                message: 'Invalid image ID',
            });
        }

        const image = await getImageById(id);

        if (!image) {
            return res.status(404).json({
                status: 'error',
                message: 'Image not found',
            });
        }

        const imagePath = path.resolve(image.file_path);
        const imageDir = path.resolve(process.env.IMAGE_DIR || '/data/images');

        if (path.dirname(imagePath) !== imageDir) {
            return res.status(400).json({
                status: 'error',
                message: 'Invalid image path',
            });
        }

        if (!fs.existsSync(imagePath)) {
            return res.status(404).json({
                status: 'error',
                message: 'Image file not found',
            });
        }

        res.type('jpg');
        res.sendFile(imagePath);

    } catch (error) {
        console.error('Image retrieval failed:', error);

        res.status(500).json({
            status: 'error',
            message: 'Failed to retrieve image',
        });
    }
});

module.exports = router;
