#pragma once
// Pure-data triangle BVH for ray casting (generic segmentation, #1179).
//
// No Ogre, no Qt: positions + indices in, nearest hit out. Built once per mesh
// by median split on the longest centroid axis (leaves of <= 4 triangles),
// which is plenty for the shape-diameter rays it exists for. Kept generic so
// other pure-data code (bakes, picking) can reuse it.

#include <cstdint>
#include <vector>

namespace GenericSeg {

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(const Vec3& v);
Vec3 normalized(const Vec3& v);   // zero vector stays zero

class TriangleBvh {
public:
    struct Hit {
        int face = -1;     // triangle index (index / 3)
        float t = 0;       // distance along the (unit) ray direction
        float u = 0, v = 0; // barycentrics of corners 1 and 2
    };

    // `positions` = xyz per vertex, `indices` = 3 per triangle. The data is
    // copied, so the arrays need not outlive the BVH.
    void build(const float* positions, int vertexCount,
               const uint32_t* indices, int indexCount);

    // Nearest hit with t in (tMin, tMax). `ignoreFace` (>= 0) is skipped, so a
    // ray leaving a surface does not hit its own triangle. Both triangle sides
    // are hit.
    bool intersect(const Vec3& origin, const Vec3& dir, float tMin, float tMax,
                   Hit& out, int ignoreFace = -1) const;

    int triangleCount() const { return int(m_tris.size()); }
    Vec3 faceNormal(int face) const;   // unit, from the winding

private:
    struct Tri { Vec3 a, b, c; };
    struct Node {
        Vec3 lo, hi;
        int left = -1, right = -1;   // children (internal node)
        int first = 0, count = 0;    // range in m_order (leaf when count > 0)
    };
    int buildNode(int first, int count, std::vector<Vec3>& centroids);
    static bool hitBox(const Node& n, const Vec3& o, const Vec3& invDir, float tMin, float tMax);

    std::vector<Tri> m_tris;
    std::vector<int> m_order;
    std::vector<Node> m_nodes;
};

} // namespace GenericSeg
