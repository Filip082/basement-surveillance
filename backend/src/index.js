const express = require('express');

const { saveImageMetadata } = require('./services/imageRepository');
const { initializeTelemetry } = require('./services/telemetryService');
const entranceRouter = require('./routes/entrance');
const imagesRouter = require('./routes/images');
const telemetryRouter = require('./routes/telemetry');

const app = express();
app.use(express.json());

const PORT = process.env.PORT || 8000;

app.get('/health', (req, res) => {
    res.status(200).json({
        status: 'ok',
        message: 'Backend is running',
    });
});

app.use('/entrance', entranceRouter);
app.use('/images', imagesRouter);
app.use('/telemetry', telemetryRouter);

/////////////////////////////////////////////////////
const path = require('path');

app.get('/test/images', (req, res) => {
    res.sendFile(
        path.resolve(__dirname, '../test/sample.jpg')
    );
});

const { downloadImage } = require('./services/imageService');


app.post('/test/download', async (req, res) => {
    const eventTimestamp = new Date();

    try {
        const imageUrl = process.env.ROUTER_IMAGE_URL;

        const result = await downloadImage(imageUrl);

        console.log('Image saved:', result.filePath);

        const image = await saveImageMetadata(
            result.filePath,
            eventTimestamp
        );

        console.log('Image metadata saved:', image);

        res.status(200).json({
            status: 'success',
            image,
            filename: result.filename,
            size: result.size,
        });

    } catch (error) {
        console.error('Image download or database save failed:', error);

        res.status(500).json({
            status: 'error',
            message: error.message,
        });
    }
});

app.post('/test/database', async (req, res) => {
    try {
        const filePath = '/data/images/test-image.jpg';
        const eventTimestamp = new Date();

        const result = await saveImageMetadata(
            filePath,
            eventTimestamp
        );

        res.status(200).json({
            status: 'success',
            image: result,
        });
    } catch (error) {
        console.error('Database save failed:', error);

        res.status(500).json({
            status: 'error',
            message: error.message,
        });
    }
});
//////////////////////////////////////////////////////

async function startServer() {
    try {
        await initializeTelemetry();

        app.listen(PORT, '0.0.0.0', () => {
            console.log(`Backend listening on port ${PORT}`);
        });
    } catch (error) {
        console.error('Backend startup failed:', error);
        process.exit(1);
    }
}

startServer();