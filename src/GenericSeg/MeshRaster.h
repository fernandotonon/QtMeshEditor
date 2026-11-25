#pragma once
// Software rasteriser for generic segmentation (#1179).
//
// Renders a normalised SegMesh (unit bounding sphere) through an orthographic
// camera into a face-id buffer, depth and barycentrics, then shades the views
// SAM sees (normals, shape diameter, matte). Done on the CPU on purpose: exact
// face ids, no GL context (headless tests and CI), and independent of the
// entity's materials.

#include "SegMesh.h"

#include <QImage>
#include <vector>

namespace GenericSeg {

struct OrthoCamera {
    Vec3 toCamera;   // unit, from the mesh centre towards the camera
    Vec3 right, up;  // unit image axes
    float halfExtent = 1.05f;   // world half-width of the image (unit sphere + margin)

    // Camera looking at the origin from `toCamera`; `upHint` picks the roll
    // (a hint parallel to the view is replaced by another axis).
    static OrthoCamera lookingFrom(const Vec3& toCamera, const Vec3& upHint = Vec3(0, 1, 0));
};

struct Raster {
    int size = 0;
    std::vector<int> face;      // size*size, -1 = background; row 0 = top
    std::vector<float> depth;   // distance along -toCamera (smaller = nearer)
    std::vector<float> b1, b2;  // barycentrics of corners 1 and 2

    int at(int x, int y) const { return face[size_t(y) * size + x]; }
    // Visible pixel count per face (size = faceCount).
    std::vector<int> pixelsPerFace(int faceCount) const;
};

Raster rasterize(const SegMesh& mesh, const OrthoCamera& cam, int size);

// View directions spread over the sphere: 12 = icosahedron vertices,
// 20 = icosahedron face centres, anything else = a Fibonacci sphere.
std::vector<Vec3> viewDirections(int count);

// Shaded images (RGB32, background black).
QImage shadeNormals(const Raster& r, const SegMesh& mesh);   // smooth world normal -> RGB
QImage shadeFaceScalar(const Raster& r, const std::vector<float>& faceValue01);  // colour ramp
QImage shadeMatte(const Raster& r, const SegMesh& mesh, const OrthoCamera& cam); // head light

} // namespace GenericSeg
