# QtMeshEditor Website (React + Vite)

This folder contains the new landing page app.

## Local development

```bash
cd website
npm install
npm run dev
```

## Build options

### Current strategy (GitHub Pages via GitHub Actions artifact)

Use the regular Vite output and deploy `website/dist` through the Pages workflow:

```bash
cd website
npm run build
```

The workflow publishes `website/dist` via `actions/upload-pages-artifact` + `actions/deploy-pages`.

### Optional local docs output

If you want a local preview using a `docs/` output folder:

```bash
cd website
npm run build:docs
```

## Notes

- Pinned GitHub Action examples in the docs app fall back to the semver in `CMakeLists.txt` (`website/src/hooks/useQtmeshActionRef.js`). Run `../scripts/sync-doc-versions-from-cmake.sh` from the repo root after bumping `project(QtMeshEditor VERSION …)`; GitHub Pages runs the same script before `npm run build`.
- `vite.config.js` uses `base: './'` so assets resolve correctly for static hosting.
- SEO/social metadata and JSON-LD structured data live in `website/index.html`.
- Most media is referenced from existing GitHub-hosted assets to keep repo size small.
