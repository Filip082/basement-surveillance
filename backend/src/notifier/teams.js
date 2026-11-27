const { toJpegBase64 } = require('./image');

// Teams rejects messages over ~28 KB, sometimes only after answering 202.
const MAX_PAYLOAD_BYTES = 27000;
const REQUEST_TIMEOUT_MS = 10000;

// Tried in order when the card with the image is too big.
const SHRINK_STEPS = [
    { maxSide: 320, quality: 60 },
    { maxSide: 240, quality: 50 },
    { maxSide: 160, quality: 40 },
];

function buildPayload({ title, body, imageB64 }) {
    const card = {
        $schema: 'http://adaptivecards.io/schemas/adaptive-card.json',
        type: 'AdaptiveCard',
        version: '1.4',
        body: [
            {
                type: 'TextBlock',
                text: title,
                weight: 'Bolder',
                size: 'Medium',
                wrap: true,
            },
            {
                type: 'TextBlock',
                text: body,
                wrap: true,
            },
        ],
    };

    if (imageB64) {
        card.body.push({
            type: 'Image',
            url: `data:image/jpeg;base64,${imageB64}`,
            altText: title,
        });
    }

    return {
        type: 'message',
        attachments: [
            {
                contentType: 'application/vnd.microsoft.card.adaptive',
                content: card,
            },
        ],
    };
}

function payloadBytes(payload) {
    return Buffer.byteLength(JSON.stringify(payload), 'utf8');
}

// Returns a payload under maxBytes: shrinks the image, then drops it.
// The alert text itself is always sent.
async function fitPayload({ title, body, imageB64 }, maxBytes) {
    const payload = buildPayload({ title, body, imageB64 });

    if (!imageB64 || payloadBytes(payload) <= maxBytes) {
        return payload;
    }

    try {
        const original = Buffer.from(imageB64, 'base64');

        for (const step of SHRINK_STEPS) {
            const smaller = await toJpegBase64(original, step);
            const candidate = buildPayload({ title, body, imageB64: smaller });

            if (payloadBytes(candidate) <= maxBytes) {
                return candidate;
            }
        }
    } catch (error) {
        console.warn(`Teams: image re-encode failed: ${error.message}`);
    }

    console.warn('Teams: image too large for the card, sending text only');

    return buildPayload({ title, body: `${body} (image too large)` });
}

async function sendTeams(
    { title, body, imageB64 },
    { maxBytes = MAX_PAYLOAD_BYTES } = {}
) {
    // Never log this URL: its signature works as a password.
    const webhookUrl = process.env.TEAMS_WEBHOOK_URL;

    if (!webhookUrl) {
        const error = new Error('TEAMS_WEBHOOK_URL is not set');
        error.retryable = false;
        throw error;
    }

    const payload = await fitPayload({ title, body, imageB64 }, maxBytes);

    const response = await fetch(webhookUrl, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload),
        signal: AbortSignal.timeout(REQUEST_TIMEOUT_MS),
    });

    if (!response.ok) {
        const text = await response.text().catch(() => '');
        throw new Error(
            `Teams webhook returned HTTP ${response.status}: ${text.slice(0, 500)}`
        );
    }
}

// Channel entry point used by notifier/index.js.
async function send(alert) {
    let imageB64;
    let body = alert.body;

    if (alert.image) {
        try {
            imageB64 = await toJpegBase64(alert.image);
        } catch (error) {
            console.warn(`Teams: could not prepare image: ${error.message}`);
            body = `${body} (image unavailable)`;
        }
    }

    await sendTeams({ title: alert.title, body, imageB64 });
}

module.exports = {
    MAX_PAYLOAD_BYTES,
    buildPayload,
    sendTeams,
    send,
};
