const express = require('express');

const {
    saveBatteryTelemetry,
    getLastBattery,
} = require('../services/telemetryService');

const router = express.Router();

router.post('/', async (req, res) => {
    const { battery } = req.body;

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
        const telemetry = await saveBatteryTelemetry(battery);

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

module.exports = router;
