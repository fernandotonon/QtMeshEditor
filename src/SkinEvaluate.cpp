#include "SkinEvaluate.h"
#include "GeodesicVoxelBind.h"
#include "SkinMetrics.h"
#include "SkinWeightsPost.h"

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSubMesh.h>
#include <OgreSkeleton.h>
#include <OgreBone.h>

#include <QJsonArray>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>
#include <vector>

namespace {

bool extractPositions(Ogre::VertexData* vd, std::vector<float>& out)
{
    const auto* posElem =
        vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    if (!posElem) return false;
    auto vbuf = vd->vertexBufferBinding->getBuffer(posElem->getSource());
    if (!vbuf || vd->vertexCount == 0) return false;
    out.resize(static_cast<size_t>(vd->vertexCount) * 3);
    const size_t stride = vbuf->getVertexSize();
    auto* base = static_cast<unsigned char*>(
        vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    for (size_t i = 0; i < vd->vertexCount; ++i) {
        float* p = nullptr;
        posElem->baseVertexPointerToElement(base + i * stride, &p);
        out[3 * i + 0] = p[0];
        out[3 * i + 1] = p[1];
        out[3 * i + 2] = p[2];
    }
    vbuf->unlock();
    return true;
}

void appendIndices(Ogre::IndexData* id, std::uint32_t vertexOffset,
                   std::vector<std::uint32_t>& out)
{
    if (!id || !id->indexBuffer || id->indexCount == 0) return;
    auto ibuf = id->indexBuffer;
    const auto* base = static_cast<const unsigned char*>(
        ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    base += id->indexStart * ibuf->getIndexSize();
    out.reserve(out.size() + id->indexCount);
    if (ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT) {
        const auto* p = reinterpret_cast<const std::uint32_t*>(base);
        for (size_t i = 0; i < id->indexCount; ++i)
            out.push_back(p[i] + vertexOffset);
    } else {
        const auto* p = reinterpret_cast<const std::uint16_t*>(base);
        for (size_t i = 0; i < id->indexCount; ++i)
            out.push_back(p[i] + vertexOffset);
    }
    ibuf->unlock();
}

// Scatter one owner's assignment list into the combined weights
// array (top-8 per vertex, largest kept).
void scatterAssignments(const Ogre::Mesh::VertexBoneAssignmentList& list,
                        std::uint32_t baseVertex,
                        std::vector<SkinWeights::VertexWeights>& weights)
{
    for (const auto& kv : list) {
        const std::size_t v = baseVertex + kv.second.vertexIndex;
        if (v >= weights.size()) continue;
        SkinWeights::VertexWeights& vw = weights[v];
        if (vw.count < 8) {
            vw.boneIndices[vw.count] = kv.second.boneIndex;
            vw.weights[vw.count]     = kv.second.weight;
            ++vw.count;
        } else {
            // Replace the smallest if this one is bigger.
            int minIdx = 0;
            for (int i = 1; i < 8; ++i)
                if (vw.weights[i] < vw.weights[minIdx]) minIdx = i;
            if (kv.second.weight > vw.weights[minIdx]) {
                vw.boneIndices[minIdx] = kv.second.boneIndex;
                vw.weights[minIdx]     = kv.second.weight;
            }
        }
    }
}

QJsonObject histogramToJson(const SkinMetrics::InfluenceHistogram& h)
{
    QJsonObject obj;
    QJsonArray counts;
    for (int c : h.counts) counts.push_back(c);
    obj["counts"]            = counts;   // index = influence count
    obj["averageInfluences"] = h.averageInfluences;
    obj["maxInfluences"]     = h.maxInfluences;
    return obj;
}

} // namespace

bool SkinEvaluate::extract(Ogre::Entity* entity, EvalData& out,
                           QString* error)
{
    auto fail = [&](const QString& msg) {
        if (error) *error = msg;
        return false;
    };
    out = {};
    if (!entity || !entity->getMesh()) return fail(QStringLiteral("no entity"));
    Ogre::MeshPtr mesh = entity->getMesh();
    Ogre::Skeleton* skel = mesh->getSkeleton().get();
    if (!skel) return fail(QStringLiteral("mesh has no skeleton attached"));

    out.totalBones = skel->getNumBones();
    for (unsigned short bi = 0; bi < skel->getNumBones(); ++bi) {
        Ogre::Bone* bone = skel->getBone(bi);
        out.boneNames << (bone ? QString::fromStdString(bone->getName())
                               : QString());
    }

    // Shared block first, then per-submesh blocks — the same owner
    // order computeAndApply uses.
    if (mesh->sharedVertexData) {
        bool anyShared = false;
        for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si)
            if (mesh->getSubMesh(si) && mesh->getSubMesh(si)->useSharedVertices) {
                anyShared = true;
                break;
            }
        std::vector<float> pos;
        if (anyShared && extractPositions(mesh->sharedVertexData, pos)) {
            const auto base = std::uint32_t(out.positions.size() / 3);
            for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
                Ogre::SubMesh* sub = mesh->getSubMesh(si);
                if (sub && sub->useSharedVertices)
                    appendIndices(sub->indexData, base, out.indices);
            }
            out.positions.insert(out.positions.end(), pos.begin(), pos.end());
            out.weights.resize(out.positions.size() / 3);
            scatterAssignments(mesh->getBoneAssignments(), base, out.weights);
        }
    }
    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (!sub || sub->useSharedVertices || !sub->vertexData) continue;
        std::vector<float> pos;
        if (!extractPositions(sub->vertexData, pos)) continue;
        const auto base = std::uint32_t(out.positions.size() / 3);
        appendIndices(sub->indexData, base, out.indices);
        out.positions.insert(out.positions.end(), pos.begin(), pos.end());
        out.weights.resize(out.positions.size() / 3);
        scatterAssignments(sub->getBoneAssignments(), base, out.weights);
    }

    if (out.positions.empty())
        return fail(QStringLiteral("mesh has no readable vertex data"));
    return true;
}

QJsonObject SkinEvaluate::evaluate(Ogre::Entity* entity, int voxelResolution,
                                   QString* error)
{
    QJsonObject report;
    EvalData data;
    if (!extract(entity, data, error)) return report;

    const int vertexCount = int(data.positions.size() / 3);
    report["vertices"]   = vertexCount;
    report["triangles"]  = int(data.indices.size() / 3);
    report["totalBones"] = data.totalBones;

    int weighted = 0;
    for (const auto& vw : data.weights)
        if (vw.count > 0) ++weighted;
    report["weightedVertices"] = weighted;

    report["influenceHistogram"] =
        histogramToJson(SkinMetrics::influenceHistogram(data.weights));

    const auto adjacency = SkinWeightsPost::buildAdjacency(
        vertexCount, data.indices.data(), data.indices.size());
    const double energy = SkinMetrics::laplacianEnergy(data.weights, adjacency);
    if (energy >= 0.0) report["weightSmoothnessEnergy"] = energy;

    // Geodesic bleed of the EXISTING weights: build the voxel field
    // from the skeleton's bind pose and check every committed weight
    // for geodesic locality. Volume-less meshes skip the metric.
    {
        Ogre::Skeleton* skel = entity->getMesh()->getSkeleton().get();
        skel->reset(true);
        std::vector<SkinWeights::BoneSegment> bones;
        for (unsigned short bi = 0; bi < skel->getNumBones(); ++bi) {
            Ogre::Bone* bone = skel->getBone(bi);
            if (!bone) {
                bones.push_back({});
                continue;
            }
            const Ogre::Vector3 head = bone->_getDerivedPosition();
            Ogre::Vector3 tail = head;
            int kept = 0;
            Ogre::Vector3 sum(0, 0, 0);
            for (unsigned short c = 0; c < bone->numChildren(); ++c) {
                auto* child = dynamic_cast<Ogre::Bone*>(bone->getChild(c));
                if (!child) continue;
                sum += child->_getDerivedPosition();
                ++kept;
            }
            if (kept > 0) tail = sum / Ogre::Real(kept);
            bones.push_back({ head.x, head.y, head.z, tail.x, tail.y, tail.z });
        }
        SkinWeightsOptions gvbOpts;
        gvbOpts.voxelResolution      = voxelResolution;
        gvbOpts.maxInfluenceDistance = 0;   // full field — locality only
        std::vector<SkinWeights::VertexWeights> unused;
        std::vector<std::vector<int>> allowed;
        const auto gvb = GeodesicVoxelBind::compute(
            data.positions.data(), vertexCount,
            data.indices.data(), data.indices.size(),
            bones, gvbOpts, unused, &allowed);
        if (gvb.ok) {
            const double bleed =
                SkinWeightsPost::bleedFraction(data.weights, allowed);
            if (bleed >= 0.0) report["bleedFraction"] = bleed;
        } else {
            report["bleedFractionSkipped"] = gvb.error;
        }
    }

    return report;
}

QJsonObject SkinEvaluate::compare(Ogre::Entity* entity,
                                  Ogre::Entity* reference,
                                  QString* error)
{
    QJsonObject report;
    EvalData ours, ref;
    if (!extract(entity, ours, error)) return report;
    if (!extract(reference, ref, error)) return report;

    // Match reference vertices by position (reference re-exports
    // reorder / split vertices) via a quantised spatial hash.
    const int oursCount = int(ours.positions.size() / 3);
    const int refCount  = int(ref.positions.size() / 3);
    double mn[3] = { std::numeric_limits<double>::max(),
                     std::numeric_limits<double>::max(),
                     std::numeric_limits<double>::max() };
    double mx[3] = { -mn[0], -mn[1], -mn[2] };
    for (int v = 0; v < refCount; ++v)
        for (int a = 0; a < 3; ++a) {
            mn[a] = std::min<double>(mn[a], ref.positions[3 * v + a]);
            mx[a] = std::max<double>(mx[a], ref.positions[3 * v + a]);
        }
    const double diag = std::sqrt(std::pow(mx[0] - mn[0], 2)
                                  + std::pow(mx[1] - mn[1], 2)
                                  + std::pow(mx[2] - mn[2], 2));
    const double cell = std::max(diag * 1e-4, 1e-9);
    auto key = [&](double x, double y, double z) {
        const long long kx = llround(x / cell);
        const long long ky = llround(y / cell);
        const long long kz = llround(z / cell);
        return (kx * 73856093LL) ^ (ky * 19349663LL) ^ (kz * 83492791LL);
    };
    std::unordered_multimap<long long, int> refByCell;
    refByCell.reserve(std::size_t(refCount));
    for (int v = 0; v < refCount; ++v)
        refByCell.emplace(key(ref.positions[3 * v], ref.positions[3 * v + 1],
                              ref.positions[3 * v + 2]), v);

    // Bone-name → handle map on the reference side.
    std::map<QString, int> refBoneByName;
    for (int b = 0; b < ref.boneNames.size(); ++b)
        refBoneByName[ref.boneNames[b]] = b;

    long long matched = 0;
    double sumL1 = 0.0, maxL1 = 0.0;
    std::map<QString, double> boneDiff;   // name → accumulated |Δw|
    const double tol = cell * 2.0;

    // Per-vertex weight vector by BONE NAME (weights on unnamed /
    // out-of-range handles are dropped).
    auto namedWeights = [](const EvalData& d, int v) {
        std::map<QString, double> out;
        const auto& vw = d.weights[v];
        for (int i = 0; i < vw.count; ++i)
            if (vw.boneIndices[i] >= 0 && vw.boneIndices[i] < d.boneNames.size())
                out[d.boneNames[vw.boneIndices[i]]] += vw.weights[i];
        return out;
    };
    auto l1Diff = [](const std::map<QString, double>& a,
                     const std::map<QString, double>& b,
                     std::map<QString, double>* perBone) {
        double l1 = 0.0;
        for (const auto& [name, w] : a) {
            const auto it = b.find(name);
            const double rw = (it != b.end()) ? it->second : 0.0;
            const double d = std::abs(w - rw);
            l1 += d;
            if (perBone) (*perBone)[name] += d;
        }
        for (const auto& [name, w] : b) {
            if (a.find(name) == a.end()) {
                l1 += w;
                if (perBone) (*perBone)[name] += w;
            }
        }
        return l1;
    };

    for (int v = 0; v < oursCount; ++v) {
        const double px = ours.positions[3 * v + 0];
        const double py = ours.positions[3 * v + 1];
        const double pz = ours.positions[3 * v + 2];
        // Probe the vertex's cell (+ tolerate quantisation edges by
        // checking the 27-neighbourhood). Collect every candidate at
        // the best distance — split/seam/contact vertices are exact
        // position duplicates with different weights, and the fair
        // match among equidistant duplicates is the one minimising
        // the weight difference (otherwise a hand vertex "matches"
        // the coincident prop vertex and reports a spurious diff).
        std::vector<int> best;
        double bestD = tol * tol;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const long long k = key(px + dx * cell, py + dy * cell,
                                            pz + dz * cell);
                    auto range = refByCell.equal_range(k);
                    for (auto it = range.first; it != range.second; ++it) {
                        const int rv = it->second;
                        const double ddx = ref.positions[3 * rv]     - px;
                        const double ddy = ref.positions[3 * rv + 1] - py;
                        const double ddz = ref.positions[3 * rv + 2] - pz;
                        const double d = ddx * ddx + ddy * ddy + ddz * ddz;
                        if (d < bestD - 1e-18) {
                            bestD = d;
                            best.assign(1, rv);
                        } else if (std::abs(d - bestD) <= 1e-18) {
                            best.push_back(rv);
                        }
                    }
                }
        if (best.empty()) continue;
        ++matched;

        const auto wOurs = namedWeights(ours, v);
        int bestRv = best.front();
        if (best.size() > 1) {
            double bestL1 = std::numeric_limits<double>::max();
            for (const int rv : best) {
                const double l1 = l1Diff(wOurs, namedWeights(ref, rv), nullptr);
                if (l1 < bestL1) { bestL1 = l1; bestRv = rv; }
            }
        }
        const double l1 = l1Diff(wOurs, namedWeights(ref, bestRv), &boneDiff);
        sumL1 += l1;
        maxL1 = std::max(maxL1, l1);
    }

    report["verticesCompared"]   = double(matched);
    report["verticesUnmatched"]  = double(oursCount - matched);
    if (matched > 0) {
        report["meanWeightL1Diff"] = sumL1 / double(matched);
        report["maxWeightL1Diff"]  = maxL1;
    }
    // Top 10 differing bones (mean |Δw| over matched verts).
    std::vector<std::pair<QString, double>> ranked(boneDiff.begin(),
                                                   boneDiff.end());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    QJsonArray top;
    for (std::size_t i = 0; i < ranked.size() && i < 10; ++i) {
        QJsonObject e;
        e["bone"] = ranked[i].first;
        e["meanAbsDiff"] = matched > 0 ? ranked[i].second / double(matched) : 0.0;
        top.push_back(e);
    }
    report["topDifferingBones"] = top;
    return report;
}

QString SkinEvaluate::reportToText(const QJsonObject& report)
{
    QString out;
    QTextStream s(&out);
    s << "Skin Evaluation\n";
    s << "===============\n";
    auto line = [&](const char* label, const QString& key) {
        if (!report.contains(key)) return;
        s << label << report.value(key).toVariant().toString() << "\n";
    };
    line("Vertices:            ", QStringLiteral("vertices"));
    line("Triangles:           ", QStringLiteral("triangles"));
    line("Bones:               ", QStringLiteral("totalBones"));
    line("Weighted vertices:   ", QStringLiteral("weightedVertices"));
    if (report.contains("influenceHistogram")) {
        const auto h = report["influenceHistogram"].toObject();
        s << "Avg influences:      "
          << QString::number(h["averageInfluences"].toDouble(), 'f', 2) << "\n";
        s << "Max influences:      " << h["maxInfluences"].toInt() << "\n";
    }
    if (report.contains("weightSmoothnessEnergy"))
        s << "Smoothness energy:   "
          << QString::number(report["weightSmoothnessEnergy"].toDouble(), 'f', 6)
          << "  (lower = smoother falloffs)\n";
    if (report.contains("bleedFraction"))
        s << "Bleed fraction:      "
          << QString::number(report["bleedFraction"].toDouble(), 'f', 4)
          << "  (weights not geodesically local)\n";
    if (report.contains("bleedFractionSkipped"))
        s << "Bleed fraction:      skipped ("
          << report["bleedFractionSkipped"].toString() << ")\n";
    line("Vertices compared:   ", QStringLiteral("verticesCompared"));
    line("Vertices unmatched:  ", QStringLiteral("verticesUnmatched"));
    if (report.contains("meanWeightL1Diff")) {
        s << "Mean weight L1 diff: "
          << QString::number(report["meanWeightL1Diff"].toDouble(), 'f', 4) << "\n";
        s << "Max weight L1 diff:  "
          << QString::number(report["maxWeightL1Diff"].toDouble(), 'f', 4) << "\n";
    }
    if (report.contains("topDifferingBones")) {
        const auto arr = report["topDifferingBones"].toArray();
        if (!arr.isEmpty()) {
            s << "Top differing bones:\n";
            for (const auto& e : arr) {
                const auto obj = e.toObject();
                s << "  " << obj["bone"].toString() << ": "
                  << QString::number(obj["meanAbsDiff"].toDouble(), 'f', 4)
                  << "\n";
            }
        }
    }
    return out;
}
