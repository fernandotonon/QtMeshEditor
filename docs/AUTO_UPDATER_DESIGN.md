# Auto-updater architecture (spike #440)

Parent epic: [#439](https://github.com/fernandotonon/QtMeshEditor/issues/439)

This document locks the four architecturally consequential decisions before production updater code lands. Implementation PoC lives under `src/updater/` with unit tests and fixtures.

## 1. Signature scheme

### Decision: **minisign / Ed25519 (pre-hashed BLAKE2b-512)**

| Option | Verdict |
|--------|---------|
| **minisign / Ed25519** | **Selected.** Single-purpose release signing; 32-byte pubkeys embed cleanly; `minisign` CLI is trivial in CI; signatures are sidecar `.minisig` files (no CMS overhead). |
| GPG/PGP | Rejected. Heavy runtime (GnuPG or bundled libgcrypt), awkward key distribution, overkill for “is this GitHub artifact authentic?”. |
| X.509 / codesign | Rejected for artifact signing. We already codesign/notarize *binaries* on macOS/Windows; X.509 adds PKI complexity without helping Linux portable tarballs/debs. |

### Public key placement

- **Production:** compile-time string `kProductionPublicKeyBase64` in `MinisignVerify.cpp`, kept in sync with `packaging/updater/minisign.pub` (human-readable copy for CI/docs).
- **Development / tests:** separate test keypair; only the `.pub` and `.minisig` fixtures are committed (`tests/fixtures/updater/`). Secret keys never enter git.

### Key rotation

1. Generate new keypair offline (`minisign -G`).
2. Publish new `minisign.pub` in repo + release notes; keep **previous** pubkey in binary for one release cycle (`kPreviousPublicKeys[]`).
3. CI signs with new secret (`MINISIGN_SECRET_KEY` GitHub Actions secret, base64-encoded key file).
4. If secret compromised: revoke GitHub secret, publish advisory, push rotation release; package-manager channels (Homebrew/WinGet/Snap) bypass in-app verify and remain the recovery path.

### CI signing

Implemented in `.github/workflows/deploy.yml` as the `sign-release-artifacts` job (runs on `release: published`). Downloads zip/dmg/deb assets, writes `SHA256SUMS`, signs each artifact + manifest with minisign, verifies with `packaging/updater/minisign.pub`, and uploads `*.minisig` + `SHA256SUMS` + `minisign.pub` to the GitHub Release.

**GitHub Actions secrets** (repo → Settings → Secrets and variables → Actions):

| Secret | Required | Value |
|--------|----------|-------|
| `MINISIGN_SECRET_KEY` | Yes | `base64 -w0 ~/.minisign/minisign.key` (helper: `scripts/encode-minisign-secret-for-github.sh`) |
| `MINISIGN_PASSWORD` | Only if the secret key is password-encrypted | The minisign key passphrase. CI passes it on stdin when signing. |

Production public key: `packaging/updater/minisign.pub` (embedded in `MinisignVerify::productionPublicKeyBase64()`).

### PoC status

- `MinisignVerify::verifyFile()` implements minisign’s dual-signature check (file + trusted comment) via **libsodium** (same algorithms as the CLI).
- Linux-only in this spike; Windows/macOS return `Unsupported` until #445 wires cross-platform static libsodium.
- Tests: `MinisignVerify_test` verifies good/tampered/bad-key against `tests/fixtures/updater/release-3.5.3-readme.md` (content from GitHub tag `3.5.3`).

---

## 2. Relauncher binary

### Decision: **Separate CMake target, minimal native stub per platform**

| Platform | Approach |
|----------|----------|
| **Windows** | Static `qtmesh-relauncher.exe` — **no Qt**, only `kernel32`: copy/move staged tree, `CreateProcess` new `QtMeshEditor.exe`, `WaitForSingleObject`, exit. Lives next to main binary. |
| **macOS** | Small Mach-O CLI **or** `/bin/mv` + `/usr/bin/open` shell-out. **Preference:** tiny native relauncher (consistent logging, no shell injection, same Sentry breadcrumbs as Windows). |
| **Linux portable** | **Shell script** `qtmesh-relauncher.sh` for spike/production MVP (AppImage/tarball installs already ship shell helpers). Optional static binary later if hardening is needed. |

### Build integration

```cmake
add_subdirectory(src/updater/relauncher)  # #446–448
```

- Windows: MinGW static link, `-nostdlib` not required (keep MSVCRT).
- Installed alongside `QtMeshEditor`; updater spawns relauncher **before** quitting so files are not locked (#439 epic risk table).

### Flow

```
UpdaterController → stage to AppData/updater/staging/<tag>/ 
                 → spawn relauncher(staging, installDir, exePath)
                 → QApplication::quit()
Relauncher       → atomic rename swap (Windows: move aside old, move in new)
                 → start new process
                 → exit 0 (or rollback on failure)
```

---

## 3. Install-flavor detection

### Decision: **`InstallFlavor::detect(applicationFilePath)`**

Enum (`src/updater/InstallFlavor.h`):

`Portable | Homebrew | WinGet | Snap | Flatpak | Debian | Docker | Unknown`

Only **`Portable`** enables download/install. All others show `updateCommandHint()` (brew/winget/snap/apt/flatpak/docker).

### Detection rules (canonical order)

| Flavor | Signal |
|--------|--------|
| **Docker** | `/.dockerenv` exists (Linux). |
| **Snap** | Path contains `/snap/qtmesheditor/` or `/snap/bin/qtmesheditor`. |
| **Flatpak** | Path under `/var/lib/flatpak/` or `~/.local/share/flatpak/`. |
| **Debian** | Binary in `/usr/bin` or `/bin` (apt `.deb` install). |
| **Homebrew** | macOS: `/Applications/Homebrew/…`, `Caskroom/qtmesheditor/`, or `.app/Contents/Resources/.brew` sentinel. |
| **WinGet** | Registry uninstall key `FernandoTonon.QtMeshEditor`, or path under `%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\WinGet`. |
| **Portable** | macOS: `/Applications/QtMeshEditor.app`; Linux: `/opt/QtMeshEditor/`; Windows: zip layout / MSI registry `QtMeshEditor` without WinGet key. |
| **Unknown** | CI trees, dev `build_local/`, ambiguous paths — **no self-update** (fail closed). |

`ENABLE_AUTO_UPDATER=OFF` for snap/flatpak/deb/docker CI builds (#439) so the code path is absent in those artifacts.

### PoC status

- Implemented in `InstallFlavor.cpp` with platform unit tests asserting synthetic paths + CI-like `Unknown` on each OS.

---

## 4. Channel taxonomy

| Channel | Source | Selection rule |
|---------|--------|----------------|
| **Stable** | GitHub Releases with `prerelease: false` | Default. Tag `X.Y.Z` (no `v` prefix per project policy). |
| **Beta** | GitHub pre-releases | Tag pattern `X.Y.Z-beta.N` (semver pre-release). User opt-in in settings (#449). |
| **Nightly** | `workflow_dispatch` artifacts | **Deferred (#452).** No GitHub Release entry — needs object storage manifest (`nightly.json` + signed SHA256 list). Spike records intent only. |

Version comparison reuses `UpdateVersion::compare()` (#442) — normalises optional `v`, numeric `QVersionNumber`, strips `-beta`/`-rc` for stable channel comparisons.

---

## Follow-up issues

| Issue | Work |
|-------|------|
| [#441](https://github.com/fernandotonon/QtMeshEditor/issues/441) | `UpdaterController` + GitHub JSON |
| [#442](https://github.com/fernandotonon/QtMeshEditor/issues/442) | Semver compare (partially done — `UpdateVersion`) |
| [#443](https://github.com/fernandotonon/QtMeshEditor/issues/443) | Wire `InstallFlavor` into UX |
| [#444–445](https://github.com/fernandotonon/QtMeshEditor/issues/444) | Download + verify pipeline |
| [#446–448](https://github.com/fernandotonon/QtMeshEditor/issues/446) | Relauncher + per-platform install |
| **New PR** | `deploy.yml` minisign signing (section above) |

---

## Files added in this spike

| Path | Purpose |
|------|---------|
| `docs/AUTO_UPDATER_DESIGN.md` | This document |
| `src/updater/InstallFlavor.*` | Flavor detection PoC |
| `src/updater/MinisignVerify.*` | minisign verify PoC (Linux) |
| `cmake/Libsodium.cmake` | Static libsodium fetch/build |
| `tests/fixtures/updater/*` | Signed README fixture + test pubkey |
| `scripts/generate-updater-fixtures.sh` | Regenerate fixtures locally |
