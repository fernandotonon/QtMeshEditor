# VAT demo — Godot Web export

This directory holds the Godot 4 Web (HTML5/WASM) export of
`tools/godot-vat-demo/scenes/demo_web.tscn`, embedded on the marketing
site via `<iframe>` in `website/src/App.jsx`.

## Re-exporting

After changes to the Godot demo project, regenerate the bundle:

```bash
cd tools/godot-vat-demo
godot --headless --export-release "Web"
```

The `export_presets.cfg` preset writes to `../../website/public/demo/index.html`
relative to the demo project, so the files land here automatically.

The export needs Godot 4's Web export templates installed. Get them
via `Editor → Manage Export Templates` in the Godot editor, or by
dropping the matching `.tpz` for your Godot version into
`~/Library/Application Support/Godot/export_templates/<version>/`
(macOS) or the platform equivalent.

## Sizes / threading

The preset uses `variant/thread_support = false` so the bundle runs
single-threaded — no `SharedArrayBuffer`, no COOP/COEP headers needed
on the hosting server. Trade-off: physics + scripts can't parallelize,
but the demo is GPU-bound (one VAT-driven mesh) so the difference
isn't user-visible.

Total bundle is ~38 MB (mostly the WASM runtime; the bake itself is
~2 MB).
