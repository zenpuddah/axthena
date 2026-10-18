import {defineConfig} from 'vite';
import react from '@vitejs/plugin-react';
export default defineConfig({plugins:[react()],base:'./',server:{proxy:{'/api':{target:'http://127.0.0.1:17780',changeOrigin:true}}},build:{chunkSizeWarningLimit:1200}});
