#!/bin/bash
# Updates the WinGet manifest for a new release.
# Usage: ./scripts/update-winget.sh <version>
#
# This script:
# 1. Downloads the Windows zip from GitHub Releases
# 2. Computes the SHA256 hash
# 3. Creates the WinGet manifest files
# 4. Outputs instructions for submitting to microsoft/winget-pkgs
#
# To submit automatically, install wingetcreate:
#   wingetcreate update FernandoTonon.QtMeshEditor --version <version> \
#     --urls "https://github.com/fernandotonon/QtMeshEditor/releases/download/<version>/QtMeshEditor-<version>-bin-Windows.zip" \
#     --submit --token <github-pat>

set -euo pipefail

VERSION="${1:?Usage: $0 <version>}"
PKG_ID="FernandoTonon.QtMeshEditor"
ZIP_URL="https://github.com/fernandotonon/QtMeshEditor/releases/download/${VERSION}/QtMeshEditor-${VERSION}-bin-Windows.zip"
MANIFEST_DIR="winget/manifests/f/FernandoTonon/QtMeshEditor/${VERSION}"

echo "=== Updating WinGet manifest for ${PKG_ID} v${VERSION} ==="

# Compute SHA256
echo "Downloading and computing SHA256..."
SHA256=$(curl -sL "${ZIP_URL}" | shasum -a 256 | cut -d' ' -f1 | tr 'a-f' 'A-F')
if [ -z "${SHA256}" ]; then
    echo "ERROR: Failed to compute SHA256. Is the release published?"
    exit 1
fi
echo "SHA256: ${SHA256}"

# Create manifest directory
mkdir -p "${MANIFEST_DIR}"

# Version manifest
cat > "${MANIFEST_DIR}/${PKG_ID}.yaml" << EOF
# yaml-language-server: \$schema=https://aka.ms/winget-manifest.version.1.12.0.schema.json
PackageIdentifier: ${PKG_ID}
PackageVersion: ${VERSION}
DefaultLocale: en-US
ManifestType: version
ManifestVersion: 1.12.0
EOF

# Locale manifest
cat > "${MANIFEST_DIR}/${PKG_ID}.locale.en-US.yaml" << EOF
# yaml-language-server: \$schema=https://aka.ms/winget-manifest.defaultLocale.1.12.0.schema.json
PackageIdentifier: ${PKG_ID}
PackageVersion: ${VERSION}
PackageLocale: en-US
Publisher: Fernando Tonon
PublisherUrl: https://github.com/fernandotonon
PublisherSupportUrl: https://github.com/fernandotonon/QtMeshEditor/issues
PackageName: QtMeshEditor
PackageUrl: https://github.com/fernandotonon/QtMeshEditor
License: MIT
LicenseUrl: https://github.com/fernandotonon/QtMeshEditor/blob/master/LICENSE
ShortDescription: Free 3D asset tool for indie game developers
Description: |-
  QtMeshEditor is a free 3D asset tool for indie game developers.
  Merge animations from multiple files, convert between 40+ formats,
  edit materials with AI, and inspect skeletons and bone weights.
  Supports GUI, CLI (qtmesh), and REST API via MCP server.
Tags:
  - 3d
  - mesh-editor
  - animation
  - fbx
  - gltf
  - ogre3d
  - game-development
  - material-editor
Moniker: qtmesheditor
ReleaseNotesUrl: https://github.com/fernandotonon/QtMeshEditor/releases/tag/${VERSION}
ManifestType: defaultLocale
ManifestVersion: 1.12.0
EOF

# Installer manifest
cat > "${MANIFEST_DIR}/${PKG_ID}.installer.yaml" << EOF
# yaml-language-server: \$schema=https://aka.ms/winget-manifest.installer.1.12.0.schema.json
PackageIdentifier: ${PKG_ID}
PackageVersion: ${VERSION}
InstallerType: zip
NestedInstallerType: portable
NestedInstallerFiles:
  - RelativeFilePath: QtMeshEditor.exe
    PortableCommandAlias: qtmesheditor
  - RelativeFilePath: qtmesh.exe
    PortableCommandAlias: qtmesh
Installers:
  - Architecture: x64
    InstallerUrl: ${ZIP_URL}
    InstallerSha256: ${SHA256}
ManifestType: installer
ManifestVersion: 1.12.0
EOF

echo ""
echo "=== Manifest created at ${MANIFEST_DIR}/ ==="
echo ""
echo "To submit to WinGet community repo:"
echo "  1. Fork https://github.com/microsoft/winget-pkgs"
echo "  2. Copy ${MANIFEST_DIR}/ to manifests/f/FernandoTonon/QtMeshEditor/${VERSION}/"
echo "  3. Submit a PR"
echo ""
echo "Or use wingetcreate (automated):"
echo "  wingetcreate update ${PKG_ID} --version ${VERSION} \\"
echo "    --urls '${ZIP_URL}' \\"
echo "    --submit --token \$GITHUB_TOKEN"
