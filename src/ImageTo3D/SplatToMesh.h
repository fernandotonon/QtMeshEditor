#ifndef SPLAT_TO_MESH_H
#define SPLAT_TO_MESH_H

#include <QString>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// Gaussian splats -> watertight triangle mesh (TripoSplat backend).
//
// TripoSplat (VAST-AI, MIT) generates 3D Gaussians, not a surface. Everything
// downstream in the editor (rigging, skinning, LODs, glTF/FBX export, game
// presets) needs a mesh, so this module turns the splats into one with the
// same voxel recipe QuadRemeshCore uses for non-manifold input:
//
//   1. splat every Gaussian (opacity-weighted, isotropic kernel of its mean
//      scale, clamped to [minRadiusVoxels, maxRadiusVoxels]) into a density
//      grid. The generated Gaussians are small (median scale ~0.0015 of the
//      unit box, below a 256^3 voxel) and sit ON the surface, so the density
//      is a thin SHELL, not a solid;
//   2. threshold the shell, dilate it one voxel to close pinholes, and
//      flood-fill the exterior from the grid border (6-connected). Everything
//      not reached is the solid — so the result is closed even where the
//      splat shell has gaps smaller than a voxel;
//   3. erode back one voxel (keeping the original shell so thin parts are not
//      eaten), blur, and run the native MarchingCubes at 0.5.
//
// A marching-cubes on the shell density alone would give a double-walled
// surface (inner + outer skin of the shell); the flood fill is what makes it
// one closed outward surface.
//
// Colour: ColorField answers "what colour is the surface here" for the texture
// bake (MeshGenBaker) as an opacity- and distance-weighted average of the
// nearest splats' SH-DC colour.
//
// Frames: splats are in TripoSplat's MODEL frame (Z up, centred in
// [-0.5, 0.5]^3). toEditorFrame() maps them into the editor's Y-up frame,
// facing +Z like the other generators ((x, y, z) -> (y, z, x), a proper
// rotation). The .ply writer instead uses upstream's own export transform so
// the file opens upright in SuperSplat / SparkJS (3DGS y-down convention).
//
// Pure data: no Ogre, no ONNX — unit-tested in SplatToMesh_test.cpp.
namespace SplatToMesh {

struct Splats {
    std::vector<float> xyz;       // Nx3, model frame
    std::vector<float> fdc;       // Nx3, SH degree-0 coefficients (raw)
    std::vector<float> opacity;   // N, already sigmoid-activated in [0,1]
    std::vector<float> scale;     // Nx3, activated (world units)
    std::vector<float> rotation;  // Nx4, wxyz, NOT normalised
    size_t count() const { return opacity.size(); }
};

// SH DC coefficient -> linear colour in [0,1] (3DGS convention).
inline float shDcToColor(float c) { return 0.28209479177387814f * c + 0.5f; }

struct Options {
    int   resolution      = 256;   // voxels along the longest axis
    float minOpacity      = 0.1f;  // splats below this are ignored entirely
    float shellThreshold  = 0.35f; // accumulated opacity counted as surface
    float minRadiusVoxels = 0.75f; // kernel radius clamp (voxel units)
    float maxRadiusVoxels = 3.0f;
    float blurSigma       = 0.8f;  // Gaussian blur of the solid before MC
    // Morphological closing steps that seal pinholes in the splat shell
    // before the exterior fill. 0 = auto: one step per 256 voxels, i.e. a
    // constant world size.
    int   closeSteps      = 0;
    // Called with (done, total) during splatting; return false to cancel.
    std::function<bool(int, int)> progress;
};

struct Result {
    bool ok = false;
    bool cancelled = false;
    QString error;
    std::vector<float>    positions;   // Mx3, model frame, outward winding
    std::vector<uint32_t> indices;
    float voxelSize = 0.0f;            // world size of one voxel
    int   splatsUsed = 0;
};

Result extract(const Splats& splats, const Options& opts = {});

// Rotate positions in place from TripoSplat's model frame into the editor
// frame: (x, y, z) -> (y, z, x). Measured on the goblin: Z is up and the
// subject faces +X in the model frame.
void toEditorFrame(std::vector<float>& xyz);

// Nearest-splat colour lookup on a uniform hash grid (cell = 2 voxels).
class ColorField {
public:
    // `positions` must be in the same frame as the query points.
    ColorField(const std::vector<float>& positions, const std::vector<float>& rgb,
               const std::vector<float>& opacity, float cellSize, float minOpacity = 0.1f);
    // Fill rgb (count*3) for count points; points with no splat in the
    // surrounding 3x3x3 cells widen the search ring (up to 4 rings), then fall
    // back to neutral grey.
    void sample(const float* points, size_t count, float* rgb) const;
    size_t size() const { return m_op.size(); }

private:
    long long key(int x, int y, int z) const;
    std::vector<float> m_pos, m_rgb, m_op;
    std::vector<uint32_t> m_order;      // splat ids sorted by cell
    std::vector<long long> m_keys;      // sorted cell keys (parallel to m_order)
    std::array<float, 3> m_min{};
    float m_cell = 1.0f;
};

// Write a standard 3DGS binary .ply (x y z nx ny nz f_dc_0..2 opacity
// scale_0..2 rot_0..3; opacity as a logit, scale as log), in upstream's export
// frame so it opens upright in splat viewers. Returns false on I/O error.
bool writePly(const QString& path, const Splats& splats, QString* error = nullptr);

// Read a standard 3DGS binary .ply (the layout writePly emits, which is also
// what upstream TripoSplat and most splat tools write) back into the model
// frame: inverse export transform, sigmoid opacity, exp scale. Extra
// properties (higher SH bands) are skipped. Returns an empty Splats + error
// on malformed input.
Splats readPly(const QString& path, QString* error = nullptr);

} // namespace SplatToMesh

#endif // SPLAT_TO_MESH_H
