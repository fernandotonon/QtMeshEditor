#ifndef TRELLIS2_INTERCHANGE_H
#define TRELLIS2_INTERCHANGE_H

#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <vector>

// QTM3D interchange container (epic: TRELLIS.2 backend). The Python sidecar
// (ai/trellis2/generate.py + qtm3d.py) writes the RAW TRELLIS.2 generation —
// vertices, faces and the sparse PBR attribute volume — into this trivial
// little-endian binary format, and QtMeshEditor's own C++ pipeline
// (Trellis2Bake) takes over from there. Deliberately not NPZ/GLB: no zip
// dependency, no detour through upstream export code (whose texture bake
// depends on the license-prohibited nvdiffrast — see
// docs/trellis2-dependencies.md).
//
// Layout: "QTMESH3D" magic, u32 version(=1), u32 jsonLen, UTF-8 JSON
// manifest, zero-pad to a 16-byte boundary, raw array blobs (each 16-byte
// aligned; offsets in the manifest are relative to the blob-section start).
// Manifest: { generator, formatVersion, meta{...},
//             arrays: { name: {dtype, shape, offset, byteLength} } }.
//
// Pure data (Qt-only, no Ogre/GL) so it unit-tests headlessly
// (Trellis2Interchange_test.cpp), mirroring the MeshGenBaker convention.
namespace Trellis2Interchange {

// One decoded generation. Attribute channel order in `voxelAttrs` follows the
// TRELLIS.2 pbr_attr_layout: base_color.rgb, metallic, roughness, alpha —
// 6 bytes per occupied voxel, 0..255.
struct Data {
    std::vector<float>    positions;     // Nx3, TRELLIS space (aabb ~[-0.5,0.5]^3)
    std::vector<uint32_t> indices;       // Mx3
    std::vector<uint32_t> voxelCoords;   // Lx3 integer voxel coordinates
    std::vector<uint8_t>  voxelAttrs;    // Lx6 (see channel order above)
    std::vector<uint8_t>  vertexColors;  // Nx4 rgba, optional (empty if absent)

    int   vertexCount   = 0;
    int   triangleCount = 0;
    int   voxelCount    = 0;
    int   resolution    = 0;      // voxel grid resolution (meta)
    float voxelSize     = 0.0f;   // meta; 1/resolution for TRELLIS.2
    float origin[3]     = {-0.5f, -0.5f, -0.5f};

    QJsonObject meta;             // full "meta" object (seed, preset, timings…)
};

struct ReadResult {
    bool ok = false;
    QString error;
    Data data;
};

// Parse + validate a .qtm3d file. Never throws; every failure lands in
// `error`. Validation: magic/version, manifest shape, blob bounds, index
// range, coords within the declared resolution, array-size consistency.
ReadResult read(const QString& path);

// Write `data` back out (used by unit tests and tooling; the production
// writer is the Python sidecar). Returns false + `error` on failure.
bool write(const QString& path, const Data& data, QString* error = nullptr);

// Parse a trellis.cpp `--dump-post` raw dump (the C++/GGML runtime flavor,
// issue #966) into the same Data. Binary little-endian layout:
//   i32 V, F, Mv, res;
//   f32 verts[V*3]; i32 faces[F*3]; i32 coords[Mv*3]; f32 pbr6[Mv*6]
// pbr6 channels per voxel: base_color.rgb, metallic, roughness, alpha in
// [0,1] (quantized to the u8 voxelAttrs lanes; note the QTM3D channel order
// is basecolor.rgb, metallic, roughness, alpha — identical). Geometry is in
// TRELLIS space (aabb [-0.5,0.5]^3), voxel centre at (ijk+0.5)/res - 0.5 —
// the same convention the Python sidecar emits.
ReadResult readTrellisCppDump(const QString& path);

} // namespace Trellis2Interchange

#endif // TRELLIS2_INTERCHANGE_H
