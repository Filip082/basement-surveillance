
const express = require('express');

const {
    saveBatteryTelemetry,
    getLastBattery,
    getBatteryHistory,
} = require('../services/telemetryService');

const router = express.Router();

router.post('/', async (req, res) => {
    const { id, battery } = req.body;

    if (
        typeof id !== 'string' ||
        !/^[a-zA-Z0-9_-]{1,64}$/.test(id)
    ) {
        return res.status(400).json({
            status: 'error',
            message: 'Invalid device ID',
        });
    }

    if (
        !Number.isInteger(battery) ||
        battery < 0 ||
        battery > 100
    ) {
        return res.status(400).json({
            status: 'error',
            message: 'Battery must be an integer between 0 and 100',
        });
    }

    try {
        const telemetry = await saveBatteryTelemetry(id, battery);

        res.status(200).json({
            status: 'success',
            telemetry,
        });

    } catch (error) {
        console.error('Telemetry save failed:', error);

        res.status(500).json({
            status: 'error',
            message: 'Failed to save telemetry',
        });
    }
});

router.get('/', (req, res) => {
    res.status(200).json({
        battery: getLastBattery(),
    });
});

router.get('/history', async (req, res) => {
    try {
        const days = Number(req.query.days ?? 14);

        if (!Number.isInteger(days) || days < 1 || days > 365) {
            return res.status(400).json({
                error: 'Days must be an integer between 1 and 365',
            });
        }

        const history = await getBatteryHistory(days);

        res.json({
            days,
            history,
        });
    } catch (error) {
        console.error('Error fetching battery history:', error);

        res.status(500).json({
            error: 'Failed to fetch battery history',
        });
    }
});

module.exports = router;
