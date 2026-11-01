# Flathub submission (manual)

Flathub’s [Generative AI policy](https://docs.flathub.org/docs/for-app-authors/requirements#generative-ai-policy)
forbids AI agents from opening submission PRs or authoring manifests. Open the
store PR yourself after reviewing these files.

Flathub also requires [building open-source apps from source](https://docs.flathub.org/docs/for-app-authors/requirements#building-from-source).
This draft wraps the release `.deb` (same approach as our Snap). Reviewers may
reject it and ask for a from-source Flatpak; treat the exception request as
best-effort.

## Steps

1. Fork https://github.com/flathub/flathub with **all branches** (not master-only).
2. Clone and track `new-pr`:

```bash
gh repo fork flathub/flathub --clone
cd flathub
git checkout --track origin/new-pr
git checkout -b add-io.github.fernandotonon.QtMeshEditor new-pr
```

3. Copy submission files to the branch root (flatten paths; Flathub expects the
   manifest next to local sources):

```bash
APP=io.github.fernandotonon.QtMeshEditor
SRC=/path/to/QtMeshEditor/packaging/flatpak
cp "$SRC/flathub/$APP.yml" "./$APP.yml"
cp "$SRC/flathub/flathub.json" ./flathub.json   # x86_64-only (amd64 .deb)
cp "$SRC/qtmesheditor-launcher.sh" "$SRC/qtmesh-launcher.sh" .
cp "$SRC/$APP.desktop" "$SRC/$APP.metainfo.xml" .
cp -a "$SRC/icons" .
cp -a "$SRC/flathub/licenses" .                 # LGPL-3 + MIT for share/licenses/
# Edit $APP.yml sources: use path: qtmesheditor-launcher.sh (etc.), keep the
# .deb url+sha256 entry. Bump url/sha256 when shipping a new release.
```

4. Local lint / build (recommended):

```bash
flatpak install -y flathub org.flatpak.Builder
flatpak run --command=flatpak-builder-lint org.flatpak.Builder manifest ./$APP.yml
```

5. Commit, push, and open a PR against **`new-pr`** (not `master`) titled:

`Add io.github.fernandotonon.QtMeshEditor`

6. In the PR body, explain finish-args (GPU/X11 — Ogre forces XCB, home for
   mesh files, secrets for QtMesh Cloud, network for downloads/MCP) and request
   a from-source exception if you keep the `.deb` module.

7. After reviewers are happy, comment `bot, build` on the PR.
