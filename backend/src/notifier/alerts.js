// Builds the alert messages for system events.

const CAMERA_NAME = process.env.CAMERA_NAME || 'basement-cam';

function warsawTime(date) {
    return date.toLocaleString('pl-PL', { timeZone: 'Europe/Warsaw' });
}

function entranceAlert(eventTimestamp) {
    return {
        title: 'Entrance detected',
        body: `Someone entered the basement. Camera: ${CAMERA_NAME}. ` +
            `Time: ${warsawTime(eventTimestamp)} (Europe/Warsaw). ` +
            'The photo follows when it arrives.',
    };
}

function photoAlert(eventTimestamp, imagePath) {
    return {
        title: 'Entrance photo',
        body: `Photo of the person entering the basement. Camera: ${CAMERA_NAME}. ` +
            `Received: ${warsawTime(eventTimestamp)} (Europe/Warsaw).`,
        image: imagePath,
    };
}

module.exports = {
    entranceAlert,
    photoAlert,
};
