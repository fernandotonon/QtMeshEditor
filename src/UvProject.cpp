#include "UvProject.h"

#include "EditableMesh.h"

#include <OgreMath.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using VertKey = std::pair<int, int>; // (subMesh, localVert)

struct VertKeyHash {
    size_t operator()(const VertKey& k) const noexcept
    {
        return static_cast<size_t>(k.first) * 1315423911u
               + static_cast<size_t>(k.second);
    }
};

bool triangleIncluded(const UvProject::Selection& selection, int subMesh, int localTri)
{
    if (subMesh < 0 || static_cast<size_t>(subMesh) >= selection.includeTriangle.size())
        return true;
    const auto& mask = selection.includeTriangle[static_cast<size_t>(subMesh)];
    if (mask.empty())
        return true;
    if (localTri < 0 || static_cast<size_t>(localTri) >= mask.size())
        return false;
    return mask[static_cast<size_t>(localTri)];
}

Ogre::Vector3 triangleNormal(const EditableMesh& mesh, int subMesh, const EditableTriangle& tri)
{
    const auto& verts = mesh.subMeshes()[static_cast<size_t>(subMesh)].vertices;
    const auto& a = verts[tri.indices[0]].position;
    const auto& b = verts[tri.indices[1]].position;
    const auto& c = verts[tri.indices[2]].position;
    Ogre::Vector3 n = (b - a).crossProduct(c - a);
    if (n.squaredLength() < 1e-16f)
        return Ogre::Vector3::UNIT_Y;
    n.normalise();
    return n;
}

int pickBoxPlaneAxis(const Ogre::Vector3& normal)
{
    const float ax = std::abs(normal.x);
    const float ay = std::abs(normal.y);
    const float az = std::abs(normal.z);
    if (ay >= ax && ay >= az)
        return normal.y >= 0.f ? 2 : 3; // +Y / -Y
    if (ax >= az)
        return normal.x >= 0.f ? 0 : 1; // +X / -X
    return normal.z >= 0.f ? 4 : 5;     // +Z / -Z
}

Ogre::Vector2 projectPositionOnBoxPlane(const Ogre::Vector3& pos, int planeAxis, float scale)
{
    switch (planeAxis) {
    case 0: return {pos.y * scale, pos.z * scale};
    case 1: return {pos.y * scale, pos.z * scale};
    case 2: return {pos.x * scale, pos.z * scale};
    case 3: return {pos.x * scale, pos.z * scale};
    case 4: return {pos.x * scale, pos.y * scale};
    default: return {pos.x * scale, pos.y * scale};
    }
}

void scaleUvMapAroundCenter(std::unordered_map<VertKey, Ogre::Vector2, VertKeyHash>& uvs,
                            float scale)
{
    if (std::abs(scale - 1.f) < 1e-6f)
        return;
    for (auto& kv : uvs) {
        kv.second.x = (kv.second.x - 0.5f) * scale + 0.5f;
        kv.second.y = (kv.second.y - 0.5f) * scale + 0.5f;
    }
}

void normalizeUvMap(std::unordered_map<VertKey, Ogre::Vector2, VertKeyHash>& uvs)
{
    if (uvs.empty())
        return;

    float minU = std::numeric_limits<float>::max();
    float minV = std::numeric_limits<float>::max();
    float maxU = -std::numeric_limits<float>::max();
    float maxV = -std::numeric_limits<float>::max();
    for (const auto& kv : uvs) {
        minU = std::min(minU, kv.second.x);
        minV = std::min(minV, kv.second.y);
        maxU = std::max(maxU, kv.second.x);
        maxV = std::max(maxV, kv.second.y);
    }

    const float du = maxU - minU;
    const float dv = maxV - minV;
    for (auto& kv : uvs) {
        if (du > 1e-8f)
            kv.second.x = (kv.second.x - minU) / du;
        else
            kv.second.x = 0.5f;
        if (dv > 1e-8f)
            kv.second.y = (kv.second.y - minV) / dv;
        else
            kv.second.y = 0.5f;
    }
}

Ogre::AxisAlignedBox boundsForKeys(const EditableMesh& mesh,
                                   const std::unordered_set<VertKey, VertKeyHash>& keys)
{
    Ogre::AxisAlignedBox box;
    box.setNull();
    for (const VertKey& key : keys) {
        if (key.first < 0
            || static_cast<size_t>(key.first) >= mesh.subMeshes().size()
            || key.second < 0
            || static_cast<size_t>(key.second)
                   >= mesh.subMeshes()[static_cast<size_t>(key.first)].vertices.size()) {
            continue;
        }
        box.merge(mesh.subMeshes()[static_cast<size_t>(key.first)]
                      .vertices[static_cast<size_t>(key.second)]
                      .position);
    }
    return box;
}

Ogre::Vector2 projectCylinder(const Ogre::Vector3& pos, int axis,
                              const Ogre::AxisAlignedBox& bounds, float scale)
{
    const Ogre::Vector3 center = bounds.getCenter();
    const Ogre::Vector3 min = bounds.getMinimum();
    const Ogre::Vector3 max = bounds.getMaximum();

    float u = 0.5f;
    float v = 0.5f;
    switch (axis % 3) {
    case 0: {
        const float ang = std::atan2(pos.y - center.y, pos.z - center.z);
        u = static_cast<float>(ang / (2.0 * Ogre::Math::PI) + 0.5);
        const float denom = std::max(max.x - min.x, 1e-6f);
        v = (pos.x - min.x) / denom * scale;
        break;
    }
    case 1: {
        const float ang = std::atan2(pos.x - center.x, pos.z - center.z);
        u = static_cast<float>(ang / (2.0 * Ogre::Math::PI) + 0.5);
        const float denom = std::max(max.y - min.y, 1e-6f);
        v = (pos.y - min.y) / denom * scale;
        break;
    }
    default: {
        const float ang = std::atan2(pos.x - center.x, pos.y - center.y);
        u = static_cast<float>(ang / (2.0 * Ogre::Math::PI) + 0.5);
        const float denom = std::max(max.z - min.z, 1e-6f);
        v = (pos.z - min.z) / denom * scale;
        break;
    }
    }
    return {u, v};
}

Ogre::Vector2 projectSphere(const Ogre::Vector3& pos, int axis,
                            const Ogre::AxisAlignedBox& bounds)
{
    const Ogre::Vector3 center = bounds.getCenter();
    Ogre::Vector3 dir = pos - center;
    if (dir.squaredLength() < 1e-12f)
        return {0.5f, 0.5f};
    dir.normalise();

    float u = 0.5f;
    float v = 0.5f;
    switch (axis % 3) {
    case 0:
        u = static_cast<float>(std::atan2(dir.y, dir.z) / (2.0 * Ogre::Math::PI) + 0.5);
        v = 0.5f - static_cast<float>(std::asin(std::clamp(dir.x, -1.f, 1.f))
                                       / Ogre::Math::PI);
        break;
    case 1:
        u = static_cast<float>(std::atan2(dir.x, dir.z) / (2.0 * Ogre::Math::PI) + 0.5);
        v = 0.5f - static_cast<float>(std::asin(std::clamp(dir.y, -1.f, 1.f))
                                       / Ogre::Math::PI);
        break;
    default:
        u = static_cast<float>(std::atan2(dir.x, dir.y) / (2.0 * Ogre::Math::PI) + 0.5);
        v = 0.5f - static_cast<float>(std::asin(std::clamp(dir.z, -1.f, 1.f))
                                       / Ogre::Math::PI);
        break;
    }
    return {u, v};
}

Ogre::Vector2 projectView(const Ogre::Vector3& localPos, const UvProject::Options& opts)
{
    const Ogre::Vector4 world = opts.worldMatrix * Ogre::Vector4(localPos, 1.f);
    const Ogre::Vector4 view = opts.viewMatrix * world;
    const Ogre::Vector4 clip = opts.projMatrix * view;
    if (std::abs(clip.w) < 1e-8f)
        return {0.5f, 0.5f};
    const float ndcX = clip.x / clip.w;
    const float ndcY = clip.y / clip.w;
    // Match the UV editor's V-up convention (see UVEditorPanel uvToScreen: panV - v).
    return {ndcX * 0.5f + 0.5f, ndcY * 0.5f + 0.5f};
}

Ogre::Vector2 projectResetBox(const Ogre::Vector3& pos, const Ogre::AxisAlignedBox& bounds)
{
    const Ogre::Vector3 min = bounds.getMinimum();
    const Ogre::Vector3 max = bounds.getMaximum();
    const float dx = std::max(max.x - min.x, 1e-6f);
    const float dy = std::max(max.y - min.y, 1e-6f);
    return {(pos.x - min.x) / dx, (pos.y - min.y) / dy};
}

std::unordered_map<VertKey, Ogre::Vector2, VertKeyHash>
projectBoxPerFace(const EditableMesh& mesh, const UvProject::Selection& selection, float scale)
{
    std::unordered_map<VertKey, Ogre::Vector2, VertKeyHash> out;
    for (size_t si = 0; si < mesh.subMeshes().size(); ++si) {
        const auto& sub = mesh.subMeshes()[si];
        for (size_t ti = 0; ti < sub.triangles.size(); ++ti) {
            if (!triangleIncluded(selection, static_cast<int>(si), static_cast<int>(ti)))
                continue;
            const auto& tri = sub.triangles[ti];
            const Ogre::Vector3 n = triangleNormal(mesh, static_cast<int>(si), tri);
            const int plane = pickBoxPlaneAxis(n);
            for (int c = 0; c < 3; ++c) {
                const unsigned int vi = tri.indices[c];
                const VertKey key{static_cast<int>(si), static_cast<int>(vi)};
                out[key] = projectPositionOnBoxPlane(sub.vertices[vi].position, plane, scale);
            }
        }
    }
    normalizeUvMap(out);
    scaleUvMapAroundCenter(out, scale);
    return out;
}

std::unordered_map<VertKey, Ogre::Vector2, VertKeyHash>
collectAffectedKeys(const EditableMesh& mesh, const UvProject::Selection& selection)
{
    std::unordered_map<VertKey, Ogre::Vector2, VertKeyHash> keys;
    for (size_t si = 0; si < mesh.subMeshes().size(); ++si) {
        const auto& sub = mesh.subMeshes()[si];
        if (selection.includeTriangle.size() > si
            && selection.includeTriangle[si].empty()) {
            for (size_t vi = 0; vi < sub.vertices.size(); ++vi)
                keys[{static_cast<int>(si), static_cast<int>(vi)}] = Ogre::Vector2::ZERO;
            continue;
        }
        for (size_t ti = 0; ti < sub.triangles.size(); ++ti) {
            if (!triangleIncluded(selection, static_cast<int>(si), static_cast<int>(ti)))
                continue;
            for (int c = 0; c < 3; ++c) {
                const int vi = static_cast<int>(sub.triangles[ti].indices[c]);
                keys[{static_cast<int>(si), vi}] = Ogre::Vector2::ZERO;
            }
        }
    }
    return keys;
}

std::vector<UvProject::VertChange>
applyUvMap(EditableMesh& mesh, const std::unordered_map<VertKey, Ogre::Vector2, VertKeyHash>& uvs)
{
    std::vector<UvProject::VertChange> changes;
    changes.reserve(uvs.size());
    for (const auto& kv : uvs) {
        const int si = kv.first.first;
        const int vi = kv.first.second;
        if (si < 0 || static_cast<size_t>(si) >= mesh.subMeshes().size())
            continue;
        auto& verts = mesh.subMeshes()[static_cast<size_t>(si)].vertices;
        if (vi < 0 || static_cast<size_t>(vi) >= verts.size())
            continue;

        UvProject::VertChange change;
        change.subMeshIndex = si;
        change.vertexIndex = vi;
        change.oldUv = verts[static_cast<size_t>(vi)].hasUV ? verts[static_cast<size_t>(vi)].uv
                                                            : Ogre::Vector2::ZERO;
        change.newUv = kv.second;
        verts[static_cast<size_t>(vi)].uv = change.newUv;
        verts[static_cast<size_t>(vi)].hasUV = true;
        changes.push_back(change);
    }
    return changes;
}

} // namespace

QString UvProject::modeToString(Mode mode)
{
    switch (mode) {
    case Mode::View: return QStringLiteral("view");
    case Mode::Box: return QStringLiteral("box");
    case Mode::Cylinder: return QStringLiteral("cylinder");
    case Mode::Sphere: return QStringLiteral("sphere");
    case Mode::ResetBox: return QStringLiteral("reset_box");
    }
    return QStringLiteral("unknown");
}

UvProject::Report UvProject::project(EditableMesh& mesh, const Selection& selection,
                                     const Options& opts)
{
    Report report;
    if (mesh.subMeshes().empty()) {
        report.error = QStringLiteral("Mesh has no submeshes");
        return report;
    }

    if (opts.mode == Mode::View && !opts.hasViewMatrices) {
        report.error = QStringLiteral("View projection requires a viewport camera");
        return report;
    }

    std::unordered_map<VertKey, Ogre::Vector2, VertKeyHash> projected;

    switch (opts.mode) {
    case Mode::Box:
        projected = projectBoxPerFace(mesh, selection, opts.boxScale);
        break;
    default: {
        projected = collectAffectedKeys(mesh, selection);
        if (projected.empty()) {
            report.error = QStringLiteral("No geometry in projection selection");
            return report;
        }

        std::unordered_set<VertKey, VertKeyHash> keySet;
        for (const auto& kv : projected)
            keySet.insert(kv.first);
        const Ogre::AxisAlignedBox bounds = boundsForKeys(mesh, keySet);

        for (auto& kv : projected) {
            const int si = kv.first.first;
            const int vi = kv.first.second;
            const Ogre::Vector3& pos =
                mesh.subMeshes()[static_cast<size_t>(si)].vertices[static_cast<size_t>(vi)].position;
            switch (opts.mode) {
            case Mode::Cylinder:
                kv.second = projectCylinder(pos, opts.axis, bounds, opts.boxScale);
                break;
            case Mode::Sphere:
                kv.second = projectSphere(pos, opts.axis, bounds);
                break;
            case Mode::ResetBox:
                kv.second = projectResetBox(pos, bounds);
                break;
            case Mode::View:
                kv.second = projectView(pos, opts);
                break;
            default:
                break;
            }
        }

        if (opts.mode == Mode::Cylinder) {
            normalizeUvMap(projected);
            scaleUvMapAroundCenter(projected, opts.boxScale);
        } else if (opts.mode == Mode::Sphere || opts.mode == Mode::ResetBox)
            normalizeUvMap(projected);
        break;
    }
    }

    if (projected.empty()) {
        report.error = QStringLiteral("Projection produced no UV changes");
        return report;
    }

    report.changes = applyUvMap(mesh, projected);
    report.vertsChanged = static_cast<int>(report.changes.size());
    report.applied = report.vertsChanged > 0;
    if (!report.applied)
        report.error = QStringLiteral("Projection produced no UV changes");
    return report;
}
