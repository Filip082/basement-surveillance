
const fs = require('fs').promises;
const path = require('path');

const SETTINGS_DIR = '/data/settings';
const SETTINGS_FILE = path.join(SETTINGS_DIR, 'storage.json');

const DEFAULT_LIMIT_GB = 16;
const MIN_LIMIT_GB = 1;
const MAX_LIMIT_GB = 16;

async function getStorageLimit() {
    try {
        const content = await fs.readFile(SETTINGS_FILE, 'utf8');
        const settings = JSON.parse(content);

        if (
            Number.isInteger(settings.limitGB) &&
            settings.limitGB >= MIN_LIMIT_GB &&
            settings.limitGB <= MAX_LIMIT_GB
        ) {
            return settings.limitGB;
        }

        return DEFAULT_LIMIT_GB;
    } catch (error) {
        if (error.code !== 'ENOENT') {
            throw error;
        }

        await setStorageLimit(DEFAULT_LIMIT_GB);
        return DEFAULT_LIMIT_GB;
    }
}

async function setStorageLimit(limitGB) {
    if (
        !Number.isInteger(limitGB) ||
        limitGB < MIN_LIMIT_GB ||
        limitGB > MAX_LIMIT_GB
    ) {
        throw new Error('Storage limit must be between 1 and 16 GB');
    }

    await fs.mkdir(SETTINGS_DIR, { recursive: true });

    const temporaryFile = `${SETTINGS_FILE}.tmp`;

    await fs.writeFile(
        temporaryFile,
        JSON.stringify({ limitGB }, null, 2),
        'utf8'
    );

    await fs.rename(temporaryFile, SETTINGS_FILE);

    return limitGB;
}

module.exports = {
    getStorageLimit,
    setStorageLimit,
    MAX_LIMIT_GB,
    MIN_LIMIT_GB,
};
