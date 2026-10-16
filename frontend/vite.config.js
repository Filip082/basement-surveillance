import react from '@vitejs/plugin-react'
import { defineConfig } from 'vite'

// https://vite.dev/config/
// FRONTEND_PORT and API_PROXY_TARGET come from the root .env (loaded by docker compose)
export default defineConfig({
  plugins: [react()],
  server: {
    host: true, // listen on 0.0.0.0 inside the container
    port: Number(process.env.FRONTEND_PORT),
    strictPort: true,
    proxy: {
      '/api': process.env.API_PROXY_TARGET,
    },
  },
})
