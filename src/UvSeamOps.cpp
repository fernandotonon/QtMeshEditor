#include "UvSeamOps.h"

#include <QSet>
#include <QString>

#include <algorithm>

namespace UvSeamOps {

namespace {

EditableVertex copyVertex(const EditableVertex& src)
{
    return src;
}

bool edgeInTriangle(unsigned int a, unsigned int b, const EditableTriangle& tri, bool& isForward)
{
    for (int i = 0; i < 3; ++i) {
        const unsigned int v0 = tri.indices[i];
        const unsigned int v1 = tri.indices[(i + 1) % 3];
        if (v0 == a && v1 == b) {
            isForward = true;
            return true;
        }
        if (v0 == b && v1 == a) {
            isForward = false;
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<UvSeamData::EdgeKey> localEdgeKeysFromGlobal(
    const EditableMesh& mesh,
    const std::vector<std::pair<int, int>>& globalEdges,
    size_t& outSubMeshIndex)
{
    std::vector<UvSeamData::EdgeKey> keys;
    outSubMeshIndex = 0;
    if (globalEdges.empty())
        return keys;

    size_t commonSub = static_cast<size_t>(-1);
    for (const auto& e : globalEdges) {
        size_t sub = 0;
        UvSeamData::EdgeKey local = 0;
        UvSeamData::globalEdgeToLocalKey(mesh, e.first, e.second, sub, local);
        if (local == 0 && sub >= mesh.subMeshes().size())
            continue;
        if (commonSub == static_cast<size_t>(-1))
            commonSub = sub;
        else if (commonSub != sub)
            return {};
        keys.push_back(local);
    }
    outSubMeshIndex = commonSub;
    return keys;
}

EdgeSplitResult splitEdges(EditableMesh& mesh, size_t subMeshIndex,
                           const std::vector<UvSeamData::EdgeKey>& edges)
{
    EdgeSplitResult result;
    if (subMeshIndex >= mesh.subMeshes().size() || edges.empty()) {
        result.error = QStringLiteral("invalid submesh or empty edge list");
        return result;
    }

    EditableSubMesh& sub = mesh.subMeshes()[subMeshIndex];
    if (sub.vertices.empty()) {
        result.error = QStringLiteral("empty submesh");
        return result;
    }

    auto splitOne = [&](unsigned int a, unsigned int b) -> bool {
        if (a >= sub.vertices.size() || b >= sub.vertices.size() || a == b)
            return false;

        bool foundForward = false;
        for (const auto& tri : sub.triangles) {
            bool fwd = false;
            if (edgeInTriangle(a, b, tri, fwd) && fwd)
                foundForward = true;
        }
        if (!foundForward)
            return false;

        const unsigned int newA = static_cast<unsigned int>(sub.vertices.size());
        sub.vertices.push_back(copyVertex(sub.vertices[a]));
        const unsigned int newB = static_cast<unsigned int>(sub.vertices.size());
        sub.vertices.push_back(copyVertex(sub.vertices[b]));
        result.vertsAdded += 2;

        for (auto& tri : sub.triangles) {
            bool fwd = false;
            if (!edgeInTriangle(a, b, tri, fwd) || !fwd)
                continue;
            for (int i = 0; i < 3; ++i) {
                if (tri.indices[i] == a)
                    tri.indices[i] = newA;
                else if (tri.indices[i] == b)
                    tri.indices[i] = newB;
            }
        }
        for (auto& face : sub.faces) {
            for (size_t i = 0; i < face.indices.size(); ++i) {
                const size_t j = (i + 1) % face.indices.size();
                EditableTriangle probe{};
                probe.indices[0] = face.indices[i];
                probe.indices[1] = face.indices[j];
                probe.indices[2] = face.indices[j];
                bool fwd = false;
                if (!edgeInTriangle(a, b, probe, fwd) || !fwd)
                    continue;
                if (face.indices[i] == a)
                    face.indices[i] = newA;
                else if (face.indices[i] == b)
                    face.indices[i] = newB;
                if (face.indices[j] == a)
                    face.indices[j] = newA;
                else if (face.indices[j] == b)
                    face.indices[j] = newB;
            }
        }

        UvSeamData::setSeam(sub, a, b, true);
        UvSeamData::setSeam(sub, newA, newB, true);
        return true;
    };

    QSet<UvSeamData::EdgeKey> seen;
    for (UvSeamData::EdgeKey key : edges) {
        if (seen.contains(key))
            continue;
        seen.insert(key);
        const unsigned int a = static_cast<unsigned int>(key >> 32);
        const unsigned int b = static_cast<unsigned int>(key & 0xFFFFFFFFu);
        if (splitOne(a, b))
            ++result.edgesSplit;
    }

    if (!sub.faces.empty())
        triangulateFaces(sub);

    result.applied = result.edgesSplit > 0;
    if (!result.applied && result.error.isEmpty())
        result.error = QStringLiteral("no splittable edges found");
    return result;
}

EdgeSewResult sewEdges(EditableMesh& mesh, size_t subMeshIndex,
                       const std::vector<UvSeamData::EdgeKey>& edges)
{
    EdgeSewResult result;
    if (subMeshIndex >= mesh.subMeshes().size() || edges.empty()) {
        result.error = QStringLiteral("invalid submesh or empty edge list");
        return result;
    }

    EditableSubMesh& sub = mesh.subMeshes()[subMeshIndex];

    auto sewOne = [&](unsigned int a, unsigned int b) -> bool {
        if (a >= sub.vertices.size() || b >= sub.vertices.size())
            return false;
        if (!sub.vertices[a].hasUV || !sub.vertices[b].hasUV)
            return false;

        const Ogre::Vector2 avg(
            (sub.vertices[a].uv.x + sub.vertices[b].uv.x) * 0.5f,
            (sub.vertices[a].uv.y + sub.vertices[b].uv.y) * 0.5f);

        const Ogre::Vector3 pa = sub.vertices[a].position;
        const Ogre::Vector3 pb = sub.vertices[b].position;
        const float eps = 1e-6f;
        bool changed = false;
        for (size_t vi = 0; vi < sub.vertices.size(); ++vi) {
            const Ogre::Vector3& p = sub.vertices[vi].position;
            if (!sub.vertices[vi].hasUV)
                continue;
            const bool nearA = p.squaredDistance(pa) <= eps;
            const bool nearB = p.squaredDistance(pb) <= eps;
            if (!nearA && !nearB)
                continue;
            sub.vertices[vi].uv = avg;
            sub.vertices[vi].hasUV = true;
            changed = true;
        }
        if (changed)
            UvSeamData::setSeam(sub, a, b, false);
        return changed;
    };

    QSet<UvSeamData::EdgeKey> seen;
    for (UvSeamData::EdgeKey key : edges) {
        if (seen.contains(key))
            continue;
        seen.insert(key);
        const unsigned int a = static_cast<unsigned int>(key >> 32);
        const unsigned int b = static_cast<unsigned int>(key & 0xFFFFFFFFu);
        if (sewOne(a, b))
            ++result.edgesSewn;
    }

    result.applied = result.edgesSewn > 0;
    if (!result.applied && result.error.isEmpty())
        result.error = QStringLiteral("no edges could be sewn");
    return result;
}

} // namespace UvSeamOps
