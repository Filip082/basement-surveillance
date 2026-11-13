import react from '@vitejs/plugin-react'
import { defineConfig } from 'vite'

export default defineConfig({
  plugins: [react()],
  server: {
    host: true,
    port: Number(process.env.FRONTEND_PORT),
    strictPort: true,
    proxy: {
      '/api': {
        target: process.env.API_PROXY_TARGET,
        rewrite: (path) => path.replace(/^\/api/, ''),
      },
    },
  },
})