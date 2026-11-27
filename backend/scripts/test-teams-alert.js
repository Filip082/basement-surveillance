// Sends one real Teams alert with a sample image.
//
//   docker compose exec backend node scripts/test-teams-alert.js [image.jpg]
//
// Needs TEAMS_WEBHOOK_URL (set in .env).

const path = require('path');
const { sendAlert } = require('../src/notifier');

const imagePath = process.argv[2] || path.join(__dirname, '../test/sample.jpg');
const now = new Date().toLocaleString('pl-PL', { timeZone: 'Europe/Warsaw' });

async function main() {
    if (!process.env.TEAMS_WEBHOOK_URL) {
        console.error('TEAMS_WEBHOOK_URL is not set.');
        process.exit(1);
    }

    const delivered = await sendAlert({
        title: 'Test alert',
        body: `Manual test from scripts/test-teams-alert.js at ${now} (Europe/Warsaw).`,
        image: imagePath,
    });

    console.log(delivered
        ? 'Sent. Check the Teams channel: the image should appear under the text.'
        : 'Not delivered, see errors above.');
    process.exit(delivered ? 0 : 1);
}

main();
