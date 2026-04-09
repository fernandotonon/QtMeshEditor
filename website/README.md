# QtMeshEditor Website (React + Vite)

This folder contains the new landing page app.

## Local development

```bash
cd website
npm install
npm run dev
```

## Build options

### Option A (current GitHub Pages strategy: serve `docs/` from `master`)

Keep repository Pages settings pointing to `master` branch + `/docs` folder.

```bash
cd website
npm run build:docs
```

This writes the static site to `../docs` (overwriting old static HTML), so GitHub Pages continues to work without workflow changes.

### Option B (dist-based deploy with GitHub Actions)

Use the regular Vite output and deploy `website/dist` through a Pages workflow:

```bash
cd website
npm run build
```

Then publish `website/dist` via `actions/upload-pages-artifact` + `actions/deploy-pages`.

## Notes

- `vite.config.js` uses `base: './'` so assets resolve correctly for docs-based static hosting.
- SEO/social metadata and JSON-LD structured data live in `website/index.html`.
- Most media is referenced from existing GitHub-hosted assets to keep repo size small.
