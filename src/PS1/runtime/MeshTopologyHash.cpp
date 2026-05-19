#include "MeshTopologyHash.h"

#include <cstring>
#include <cmath>

namespace {

float quantize(float v, MeshDedupeMode mode)
{
    if (mode == MeshDedupeMode::Loose)
        return std::round(v * 100.0f) * 0.01f;
    return v;
}

void mixHash(quint64 &h, quint64 v)
{
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
}

void mixFloat(quint64 &h, float v, MeshDedupeMode mode)
{
    if (mode == MeshDedupeMode::Strict) {
        quint32 bits = 0;
        static_assert(sizeof(float) == sizeof(quint32));
        std::memcpy(&bits, &v, sizeof(bits));
        mixHash(h, bits);
    } else {
        const qint32 q = static_cast<qint32>(std::lround(v * 100.0f));
        mixHash(h, static_cast<quint64>(static_cast<quint32>(q)));
    }
}

} // namespace

quint64 MeshTopologyHash::hashMesh(const ReconstructedMesh &mesh, MeshDedupeMode mode)
{
    quint64 h = 0xcbf29ce484222325ULL;
    mixHash(h, static_cast<quint64>(mesh.subMeshes.size()));

    for (const ReconstructedSubMesh &sub : mesh.subMeshes) {
        mixHash(h, static_cast<quint64>(sub.vertices.size()));
        mixHash(h, static_cast<quint64>(sub.indices.size()));

        for (uint32_t idx : sub.indices) {
            mixHash(h, idx);
            if (idx >= static_cast<uint32_t>(sub.vertices.size()))
                continue;
            const ReconstructedVertex &v = sub.vertices[static_cast<int>(idx)];
            mixFloat(h, quantize(v.px, mode), mode);
            mixFloat(h, quantize(v.py, mode), mode);
            mixFloat(h, quantize(v.pz, mode), mode);
            mixFloat(h, quantize(v.u, mode), mode);
            mixFloat(h, quantize(v.v, mode), mode);
            mixHash(h, v.diffuseArgb);
        }
    }
    return h;
}

ReconstructedMesh MeshTopologyHash::centered(const ReconstructedMesh &mesh, float &cxOut, float &cyOut,
                                               float &czOut)
{
    ReconstructedMesh out = mesh;
    cxOut = cyOut = czOut = 0.0f;
    int count = 0;
    for (const ReconstructedSubMesh &sub : mesh.subMeshes) {
        for (const ReconstructedVertex &v : sub.vertices) {
            cxOut += v.px;
            cyOut += v.py;
            czOut += v.pz;
            ++count;
        }
    }
    if (count == 0)
        return out;

    cxOut /= static_cast<float>(count);
    cyOut /= static_cast<float>(count);
    czOut /= static_cast<float>(count);

    for (ReconstructedSubMesh &sub : out.subMeshes) {
        for (ReconstructedVertex &v : sub.vertices) {
            v.px -= cxOut;
            v.py -= cyOut;
            v.pz -= czOut;
        }
    }
    return out;
}
