const { describe, it, beforeEach, afterEach, mock } = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const sharp = require('sharp');

const { toJpegBase64 } = require('../src/notifier/image');
const teams = require('../src/notifier/teams');
const { sendAlert } = require('../src/notifier');

const SAMPLE_JPG = path.join(__dirname, 'sample.jpg');
const WEBHOOK = 'https://example.invalid/workflows/test?sig=secret';

function okResponse(status = 202) {
    return new Response('', { status });
}

function sentPayload(fetchMock, call = 0) {
    return JSON.parse(fetchMock.mock.calls[call].arguments[1].body);
}

function cardOf(payload) {
    return payload.attachments[0].content;
}

// Random pixels compress badly, so this JPEG cannot fit at 400 px.
async function noiseJpegB64(size = 400) {
    const pixels = Buffer.alloc(size * size * 3);
    for (let i = 0; i < pixels.length; i++) {
        pixels[i] = Math.floor(Math.random() * 256);
    }
    const jpeg = await sharp(pixels, { raw: { width: size, height: size, channels: 3 } })
        .jpeg({ quality: 70 })
        .toBuffer();
    return jpeg.toString('base64');
}

describe('teams notifier', () => {
    let fetchMock;

    beforeEach(() => {
        process.env.TEAMS_WEBHOOK_URL = WEBHOOK;
        fetchMock = mock.method(globalThis, 'fetch', async () => okResponse());
        mock.method(console, 'warn', () => {});
    });

    afterEach(() => {
        mock.restoreAll();
        delete process.env.TEAMS_WEBHOOK_URL;
    });

    it('posts an Adaptive Card 1.4 message with title and body', async () => {
        await teams.sendTeams({ title: 'Hello', body: 'World' });

        assert.equal(fetchMock.mock.callCount(), 1);
        const [url, options] = fetchMock.mock.calls[0].arguments;
        assert.equal(url, WEBHOOK);
        assert.equal(options.method, 'POST');
        assert.equal(options.headers['Content-Type'], 'application/json');
        assert.ok(options.signal instanceof AbortSignal);

        const payload = JSON.parse(options.body);
        assert.equal(payload.type, 'message');
        assert.equal(payload.attachments.length, 1);
        assert.equal(
            payload.attachments[0].contentType,
            'application/vnd.microsoft.card.adaptive'
        );

        const card = cardOf(payload);
        assert.equal(card.type, 'AdaptiveCard');
        assert.equal(card.version, '1.4');
        assert.deepEqual(card.body[0], {
            type: 'TextBlock',
            text: 'Hello',
            weight: 'Bolder',
            size: 'Medium',
            wrap: true,
        });
        assert.deepEqual(card.body[1], { type: 'TextBlock', text: 'World', wrap: true });
        assert.equal(card.body.length, 2);
    });

    it('includes the image as a data URL', async () => {
        const imageB64 = await toJpegBase64(SAMPLE_JPG);
        await teams.sendTeams({ title: 'T', body: 'B', imageB64 });

        const image = cardOf(sentPayload(fetchMock)).body[2];
        assert.equal(image.type, 'Image');
        assert.equal(image.url, `data:image/jpeg;base64,${imageB64}`);
    });

    it('send(alert) prepares the image from a file path', async () => {
        await teams.send({ title: 'T', body: 'B', image: SAMPLE_JPG });

        const image = cardOf(sentPayload(fetchMock)).body[2];
        assert.match(image.url, /^data:image\/jpeg;base64,\/9j\//);
    });

    it('shrinks an oversized image until the message fits', async () => {
        const imageB64 = await noiseJpegB64();
        const original = teams.buildPayload({ title: 'T', body: 'B', imageB64 });
        assert.ok(JSON.stringify(original).length > teams.MAX_PAYLOAD_BYTES);

        await teams.sendTeams({ title: 'T', body: 'B', imageB64 });

        const sent = fetchMock.mock.calls[0].arguments[1].body;
        assert.ok(Buffer.byteLength(sent) <= teams.MAX_PAYLOAD_BYTES);
        const card = cardOf(JSON.parse(sent));
        assert.equal(card.body[2].type, 'Image');
        assert.notEqual(card.body[2].url, `data:image/jpeg;base64,${imageB64}`);
        assert.equal(card.body[1].text, 'B');
    });

    it('falls back to text only when the image never fits', async () => {
        const imageB64 = await noiseJpegB64();
        await teams.sendTeams({ title: 'T', body: 'B', imageB64 }, { maxBytes: 1000 });

        assert.equal(fetchMock.mock.callCount(), 1);
        const card = cardOf(sentPayload(fetchMock));
        assert.equal(card.body.length, 2);
        assert.equal(card.body[1].text, 'B (image too large)');
    });

    it('treats non-2xx as failure with status and response text', async () => {
        fetchMock.mock.mockImplementation(async () => new Response('Bad card', { status: 400 }));

        await assert.rejects(
            teams.sendTeams({ title: 'T', body: 'B' }),
            /HTTP 400: Bad card/
        );
    });

    it('fails without retrying when TEAMS_WEBHOOK_URL is missing', async () => {
        delete process.env.TEAMS_WEBHOOK_URL;

        await assert.rejects(teams.sendTeams({ title: 'T', body: 'B' }), (error) => {
            assert.equal(error.retryable, false);
            return true;
        });
        assert.equal(fetchMock.mock.callCount(), 0);
    });
});

describe('sendAlert', () => {
    let errors;

    beforeEach(() => {
        process.env.TEAMS_WEBHOOK_URL = WEBHOOK;
        errors = mock.method(console, 'error', () => {});
        mock.method(console, 'warn', () => {});
    });

    afterEach(() => {
        mock.restoreAll();
        delete process.env.TEAMS_WEBHOOK_URL;
    });

    const noDelay = { retryDelaysMs: [0, 0] };

    it('retries a failing channel and succeeds on the 3rd attempt', async () => {
        let calls = 0;
        const channel = {
            name: 'flaky',
            send: async () => {
                calls++;
                if (calls < 3) throw new Error('boom');
            },
        };

        const delivered = await sendAlert({ title: 'T', body: 'B' }, { channels: [channel], ...noDelay });

        assert.equal(delivered, true);
        assert.equal(calls, 3);
        assert.equal(errors.mock.callCount(), 0);
    });

    it('gives up after 3 attempts and logs the channel name', async () => {
        let calls = 0;
        const channel = { name: 'broken', send: async () => { calls++; throw new Error('down'); } };

        const delivered = await sendAlert({ title: 'T', body: 'B' }, { channels: [channel], ...noDelay });

        assert.equal(delivered, false);
        assert.equal(calls, 3);
        assert.match(errors.mock.calls[0].arguments[0], /"broken" failed: down/);
    });

    it('returns true when at least one channel succeeds', async () => {
        const channels = [
            { name: 'broken', send: async () => { throw new Error('down'); } },
            { name: 'ok', send: async () => {} },
        ];

        assert.equal(await sendAlert({ title: 'T', body: 'B' }, { channels, ...noDelay }), true);
    });

    it('retries the Teams channel on non-2xx responses', async () => {
        let calls = 0;
        const fetchMock = mock.method(globalThis, 'fetch', async () => {
            calls++;
            return calls < 2 ? new Response('busy', { status: 503 }) : okResponse();
        });

        const delivered = await sendAlert({ title: 'T', body: 'B' }, noDelay);

        assert.equal(delivered, true);
        assert.equal(fetchMock.mock.callCount(), 2);
    });
});
