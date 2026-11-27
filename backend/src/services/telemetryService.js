
const fs = require('fs');
const path = require('path');

const LOG_DIR = process.env.TELEMETRY_LOG_DIR || '/data/logs';

let lastBattery = {};

async function initializeTelemetry() {
    await fs.promises.mkdir(LOG_DIR, { recursive: true });

    const files = await fs.promises.readdir(LOG_DIR);

    for (const file of files) {
        if (!file.endsWith('.log') || file === 'telemetry.log') {
            continue;
        }

        const id = file.slice(0, -4);
        const logFile = path.join(LOG_DIR, file);

        try {
            const content = await fs.promises.readFile(logFile, 'utf8');
            const lines = content.trim().split('\n');

            if (!lines[0]) {
                continue;
            }

            const lastEntry = JSON.parse(lines[lines.length - 1]);

            if (
                Number.isInteger(lastEntry.battery) &&
                lastEntry.battery >= 0 &&
                lastEntry.battery <= 100
            ) {
                lastBattery[id] = lastEntry.battery;
            }
        } catch (error) {
            console.error(`Error reading telemetry for ${id}:`, error.message);
        }
    }

    console.log('Restored battery states:', lastBattery);
}

async function saveBatteryTelemetry(id, battery) {
    await fs.promises.mkdir(LOG_DIR, { recursive: true });

    const receivedAt = new Date();

    const logEntry = {
        id,
        battery,
        received_at: receivedAt.toISOString(),
    };

    const logFile = path.join(LOG_DIR, `${id}.log`);

    await fs.promises.appendFile(
        logFile,
        JSON.stringify(logEntry) + '\n',
        'utf8'
    );

    lastBattery[id] = battery;

    console.log('Telemetry received:', logEntry);

    return logEntry;
}

function getLastBattery() {
    return lastBattery;
}


async function getBatteryHistory(days = 14) {
    await fs.promises.mkdir(LOG_DIR, { recursive: true });

    const cutoff = Date.now() - days * 24 * 60 * 60 * 1000;

    const files = await fs.promises.readdir(LOG_DIR);
    const history = {};

    for (const file of files) {
        if (!file.endsWith('.log') || file === 'telemetry.log') {
            continue;
        }

        const id = file.slice(0, -4);
        const logFile = path.join(LOG_DIR, file);

        try {
            const content = await fs.promises.readFile(logFile, 'utf8');

            const entries = content
                .split('\n')
                .filter(line => line.trim() !== '')
                .map(line => JSON.parse(line))
                .filter(entry => {
                    const timestamp = Date.parse(entry.received_at);

                    return (
                        Number.isInteger(entry.battery) &&
                        entry.battery >= 0 &&
                        entry.battery <= 100 &&
                        Number.isFinite(timestamp) &&
                        timestamp >= cutoff &&
                        timestamp <= Date.now()
                    );
                })
                .sort((a, b) =>
                    Date.parse(a.received_at) - Date.parse(b.received_at)
                );

            history[id] = entries.map(entry => ({
                battery: entry.battery,
                received_at: entry.received_at,
            }));
        } catch (error) {
            console.error(`Error reading history for ${id}:`, error.message);
            history[id] = [];
        }
    }

    return history;
}

module.exports = {
    initializeTelemetry,
    saveBatteryTelemetry,
    getLastBattery,
    getBatteryHistory,
};