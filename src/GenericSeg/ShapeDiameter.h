#pragma once
// Shape Diameter Function (Shapira, Shamir & Cohen-Or 2008) per face, for
// generic segmentation (#1179): local thickness of the volume under each
// face. It separates thin parts (blade, handle, leg) from thick ones where
// the surface normals are continuous. Pure data.

#include "SegMesh.h"

#include <vector>

namespace GenericSeg {

struct SdfOptions {
    int rays = 30;               // rays per face
    float coneDegrees = 120.0f;  // full cone angle around the inward normal
    int smoothIterations = 2;    // face-graph Laplacian passes after the rays
    int threads = 0;             // 0 = hardware concurrency
};

// Raw SDF per face in the mesh's normalised units (unit bounding sphere).
// Faces whose rays never reach an opposite wall (open sheets) take the mean
// of their neighbours; a mesh with no closed volume returns all zeros.
std::vector<float> shapeDiameter(const SegMesh& mesh, const TriangleBvh& bvh,
                                 const SdfOptions& opts = {});

// Map raw values to [0,1] for shading: min-max normalise, then the paper's
// log scale log(1 + a*v) / log(1 + a) (a = 4) that spreads the thin end.
std::vector<float> normalizeSdf(const std::vector<float>& raw);

} // namespace GenericSeg
