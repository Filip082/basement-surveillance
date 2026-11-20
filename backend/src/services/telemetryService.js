
const fs = require('fs');
const path = require('path');

const LOG_DIR = process.env.TELEMETRY_LOG_DIR || '/data/logs';
const LOG_FILE = path.join(LOG_DIR, 'telemetry.log');

let lastBattery = null;

async function initializeTelemetry() {
    try {
        const content = await fs.promises.readFile(LOG_FILE, 'utf8');

        const lines = content.trim().split('\n');

        if (lines.length === 0 || !lines[0]) {
            return;
        }

        const lastEntry = JSON.parse(lines[lines.length - 1]);

        if (
            Number.isInteger(lastEntry.battery) &&
            lastEntry.battery >= 0 &&
            lastEntry.battery <= 100
        ) {
            lastBattery = lastEntry.battery;
        }

        console.log('Restored battery state:', lastBattery);

    } catch (error) {
        if (error.code === 'ENOENT') {
            console.log('No telemetry log found. Waiting for first measurement.');
            return;
        }

        throw error;
    }
}

async function saveBatteryTelemetry(battery) {
    await fs.promises.mkdir(LOG_DIR, { recursive: true });

    const receivedAt = new Date();

    const logEntry = {
        battery,
        received_at: receivedAt.toISOString(),
    };

    await fs.promises.appendFile(
        LOG_FILE,
        JSON.stringify(logEntry) + '\n',
        'utf8'
    );

    lastBattery = battery;

    console.log('Telemetry received:', logEntry);

    return logEntry;
}

function getLastBattery() {
    return lastBattery;
}

module.exports = {
    initializeTelemetry,
    saveBatteryTelemetry,
    getLastBattery,
};
