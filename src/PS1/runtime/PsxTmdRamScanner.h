#ifndef PSXTMDRAMSCANNER_H
#define PSXTMDRAMSCANNER_H

#include <cstddef>
#include <cstdint>

class EmuHooks;

/**
 * Scan PS1 system RAM for Sony SDK TMD (`0x00000041` magic) mesh structures and emit each
 * unique find as a model-space mesh via `EmuHooks::onModelMesh` (#674).
 *
 * Unlike the screen-space GP0 path (`PsxOrderingTableScanner` / `PsxGp0ChainRootScanner` /
 * linear), TMD data sits in RAM in *model space* — vertex coordinates are pre-projection,
 * pre-camera-transform. The scanner therefore bypasses `MeshReconstructor::screenToModel`
 * entirely; meshes arrive in editor world units (PSX 12.4 fixed × `kTmdEditorUniformScale`
 * × 180° Z rotation, matching the on-disk TMD importer in `PS1TMD::importTmd`).
 *
 * Supports both `flag=0` (offsets relative to file-byte 12, the on-disk form) and
 * `flag=1` (offsets are absolute KSEG0 RAM addresses, the post-fixup runtime form).
 * Walks the bread-and-butter primitive set:
 *   0x20 mono tri (lit)         0x28 mono quad (lit)
 *   0x30 flat/gouraud tri (lit) 0x28 flag=4 gouraud quad
 *   0x24 textured tri (lit)     0x25 flag=1 textured tri (no light)
 *   0x34 gouraud textured tri   0x35 flag=1 gouraud textured tri (no light)
 *
 * Disabled with `QTMESH_PS1_TMD_SCANNER=0`. Defaults to enabled.
 */
class PsxTmdRamScanner
{
public:
    /** Sweep `ram` (size `byteSize`) for TMDs. Stops after `kMaxTmdsPerFrame` finds to bound
     *  per-frame cost. Returns the number of unique meshes accepted by the hooks
     *  (`EmuHooks::onModelMesh` returning true). */
    static int captureFromSystemRam(const uint8_t *ram, size_t byteSize, EmuHooks *hooks);
};

#endif // PSXTMDRAMSCANNER_H
