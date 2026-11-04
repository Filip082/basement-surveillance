const express = require('express');

const entranceRouter = require('./routes/entrance');

const app = express();

const PORT = process.env.PORT || 8000;

app.get('/health', (req, res) => {
    res.status(200).json({
        status: 'ok',
        message: 'Backend is running',
    });
});

app.use('/entrance', entranceRouter);

/////////////////////////////////////////////////////
const path = require('path');

app.get('/test/images', (req, res) => {
    res.sendFile(
        path.resolve(__dirname, '../test/sample.jpg')
    );
});

const { downloadImage } = require('./services/imageService');

app.post('/test/download', async (req, res) => {
    try {
        const imageUrl = 'http://backend:8000/test/images';

        const result = await downloadImage(imageUrl);

        console.log('Image saved:', result.filePath);

        res.status(200).json({
            status: 'success',
            ...result,
        });
    } catch (error) {
        console.error('Image download failed:', error);

        res.status(500).json({
            status: 'error',
            message: error.message,
        });
    }
});
//////////////////////////////////////////////////////

app.listen(PORT, '0.0.0.0', () => {
    console.log(`Backend listening on port ${PORT}`);
}); 