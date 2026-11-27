const express = require('express');

const { sendAlertInBackground } = require('../notifier');
const { entranceAlert } = require('../notifier/alerts');

const router = express.Router();

router.post('/', async (req, res) => {
    const eventTimestamp = new Date();

    console.log(
        'Entrance event received:',
        eventTimestamp.toISOString()
    );

    res.status(200).json({
        status: 'ok',
        event_timestamp: eventTimestamp.toISOString(),
    });

    sendAlertInBackground(entranceAlert(eventTimestamp));
    // new Date(timestamp).toLocaleString('pl-PL', {
    // timeZone: 'Europe/Warsaw'
    // });
});

module.exports = router;