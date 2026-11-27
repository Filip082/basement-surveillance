const express = require('express');
const fs = require('fs');
const path = require('path');
const { downloadImage } = require('../services/imageService');
const { getImageById, saveImageMetadata, getAllImages } = require('../services/imageRepository');

const router = express.Router();

router.post('/', async (req, res) => {
    const eventTimestamp = new Date();

    try {
        const imageUrl = process.env.ROUTER_IMAGE_URL;

        const result = await downloadImage(imageUrl);

        const image = await saveImageMetadata(
            result.filePath,
            eventTimestamp
        );

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
        const images = await getAllImages();

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
