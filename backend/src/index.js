const express = require('express');

const PORT = process.env.PORT || 8000;

const app = express();
app.use(express.json());

app.get('/api/health', (req, res) => {
  res.json({ status: 'ok' });
});

// Turns errors into short JSON replies instead of HTML + stack trace.
app.use((err, req, res, next) => {
  console.error(err);
  res.status(err.status || 500).json({ error: err.message });
});

app.listen(PORT, () => console.log(`backend listening on :${PORT}`));
