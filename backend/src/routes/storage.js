const express = require('express');

const { getDirectorySize } = require('../services/storageService');
const { getStorageLimit } = require('../services/storageSettingsService');
const { updateStorageLimit } = require('../services/storageService');

const router = express.Router();

router.get('/', async (req, res) => {
    try {
        const [sizeBytes, limitGB] = await Promise.all([
            getDirectorySize(process.env.IMAGE_DIR || '/data/images'),
            getStorageLimit(),
        ]);

        const limitBytes = limitGB * 1024 * 1024 * 1024;

        res.json({
            status: 'success',
            sizeBytes,
            sizeGB: Number((sizeBytes / (1024 ** 3)).toFixed(3)),
            limitGB,
            limitBytes,
            usagePercent: Number(((sizeBytes / limitBytes) * 100).toFixed(1)),
        });
    } catch (error) {
        console.error('Storage status failed:', error);
        res.status(500).json({
            status: 'error',
            message: 'Failed to read storage status',
        });
    }
});

router.put('/limit', async (req, res) => {
    try {
        const { limitGB } = req.body;

        if (
            !Number.isInteger(limitGB) ||
            limitGB < 1 ||
            limitGB > 16
        ) {
            return res.status(400).json({
                status: 'error',
                message: 'Limit must be an integer between 1 and 16 GB',
            });
        }

        const result = await updateStorageLimit(limitGB);

        res.json({
            status: 'success',
            ...result,
            usagePercent: Number(
                ((result.sizeBytes / result.limitBytes) * 100).toFixed(1)
            ),
        });
    } catch (error) {
        console.error('Storage limit update failed:', error);
        res.status(500).json({
            status: 'error',
            message: error.message,
        });
    }
});

module.exports = router;
