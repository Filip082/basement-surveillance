const teams = require('./teams');

// An alert is { title, body, image? } where image is a Buffer or a file path.
//
// To add a channel: create a module exporting `send(alert)` and add it here.
// send() should throw on failure; set `error.retryable = false` to skip retries.
const CHANNELS = [
    { name: 'teams', send: teams.send },
];

// Waits between the 3 attempts.
const RETRY_DELAYS_MS = [1000, 3000];

function sleep(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

async function sendWithRetry(channel, alert, retryDelaysMs) {
    for (let attempt = 1; ; attempt++) {
        try {
            return await channel.send(alert);
        } catch (error) {
            if (error.retryable === false || attempt > retryDelaysMs.length) {
                throw error;
            }

            console.warn(
                `Alert channel "${channel.name}" attempt ${attempt} failed, retrying: ${error.message}`
            );

            await sleep(retryDelaysMs[attempt - 1]);
        }
    }
}

// Resolves to true if at least one channel delivered the alert.
async function sendAlert(
    alert,
    { channels = CHANNELS, retryDelaysMs = RETRY_DELAYS_MS } = {}
) {
    const results = await Promise.allSettled(
        channels.map((channel) => sendWithRetry(channel, alert, retryDelaysMs))
    );

    results.forEach((result, i) => {
        if (result.status === 'rejected') {
            const reason = result.reason;
            console.error(
                `Alert channel "${channels[i].name}" failed: ${reason?.message ?? reason}`
            );
        }
    });

    return results.some((result) => result.status === 'fulfilled');
}

// Fire-and-forget for request handlers: never blocks, never throws.
function sendAlertInBackground(alert) {
    sendAlert(alert)
        .then((delivered) => {
            if (!delivered) {
                console.error(`Alert not delivered on any channel: ${alert.title}`);
            }
        })
        .catch((error) => {
            console.error('Alert dispatch failed:', error);
        });
}

module.exports = {
    sendAlert,
    sendAlertInBackground,
};
