const fs = require('fs').promises;
const path = require('path');
const crypto = require('crypto');

const {
    getOldestImage,
    deleteImageById,
} = require('./imageRepository');

const {
    getStorageLimit,
    setStorageLimit,
} = require('./storageSettingsService');

const IMAGE_DIR = path.resolve(process.env.IMAGE_DIR || '/data/images');

let operationQueue = Promise.resolve();

function runExclusive(operation) {
    const result = operationQueue.then(operation, operation);
    operationQueue = result.catch(() => {});
    return result;
}

async function getDirectorySize(dir) {
    let totalSize = 0;

    const entries = await fs.readdir(dir, { withFileTypes: true });

    for (const entry of entries) {
        const fullPath = path.join(dir, entry.name);

        if (entry.isDirectory()) {
            totalSize += await getDirectorySize(fullPath);
        } else if (entry.isFile()) {
            const stats = await fs.stat(fullPath);
            totalSize += stats.size;
        }
    }

    return totalSize;
}

function validateImagePath(filePath) {
    const resolvedPath = path.resolve(filePath);
    const relativePath = path.relative(IMAGE_DIR, resolvedPath);

    if (
        !relativePath ||
        relativePath.startsWith('..') ||
        path.isAbsolute(relativePath)
    ) {
        throw new Error('Invalid image path');
    }

    return resolvedPath;
}

async function removeOldestImage() {
    const image = await getOldestImage();

    if (!image) {
        return false;
    }

    const imagePath = validateImagePath(image.file_path);
    const quarantinePath = `${imagePath}.deleting-${crypto.randomUUID()}`;

    await fs.rename(imagePath, quarantinePath);

    try {
        const deleted = await deleteImageById(image.id);

        if (!deleted) {
            throw new Error('Image database record was not deleted');
        }
    } catch (error) {
        await fs.rename(quarantinePath, imagePath);
        throw error;
    }

    await fs.unlink(quarantinePath);

    return true;
}

async function enforceStorageLimit(newImageSize = 0, newImagePath = null) {
    return runExclusive(async () => {
        const limitGB = await getStorageLimit();
        const limitBytes = limitGB * 1024 * 1024 * 1024;

        if (newImageSize > limitBytes) {
            if (newImagePath) {
                await fs.unlink(newImagePath).catch(() => {});
            }

            throw new Error('Image exceeds the entire storage limit');
        }

        let currentSize = await getDirectorySize(IMAGE_DIR);

        while (currentSize > limitBytes) {
            const removed = await removeOldestImage();

            if (!removed) {
                if (newImagePath) {
                    await fs.unlink(newImagePath).catch(() => {});
                }

                throw new Error(
                    'Storage limit exceeded and no more database images can be removed'
                );
            }

            currentSize = await getDirectorySize(IMAGE_DIR);
        }

        return {
            sizeBytes: currentSize,
            limitBytes,
            limitGB,
        };
    });
}

async function updateStorageLimit(limitGB) {
    return runExclusive(async () => {
        await setStorageLimit(limitGB);

        const limitBytes = limitGB * 1024 * 1024 * 1024;
        let currentSize = await getDirectorySize(IMAGE_DIR);

        while (currentSize > limitBytes) {
            const removed = await removeOldestImage();

            if (!removed) {
                throw new Error(
                    'Cannot reach the new storage limit: no more database images to remove'
                );
            }

            currentSize = await getDirectorySize(IMAGE_DIR);
        }

        return {
            limitGB,
            sizeBytes: currentSize,
            limitBytes,
        };
    });
}

module.exports = {
    getDirectorySize,
    enforceStorageLimit,
    updateStorageLimit,
};
