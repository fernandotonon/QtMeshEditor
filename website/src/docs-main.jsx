import React from 'react';
import { createRoot } from 'react-dom/client';
import DocsApp from './DocsApp';
import './styles/global.css';

createRoot(document.getElementById('root')).render(
  <React.StrictMode>
    <DocsApp />
  </React.StrictMode>
);
