#include "MeshDecimator.h"

#include <Ogre.h>
#include <OgreMesh.h>
#include <OgreSubMesh.h>
#include <OgreEntity.h>
#include <OgreMeshLodGenerator.h>
#include <OgreLodConfig.h>

#include <QJsonArray>
#include <QLocale>
#include <algorithm>
#include <cmath>

// ----- Pure-data primitives -------------------------------------------------

double MeshDecimator::reductionFromTargetTris(int currentTris, int targetTris)
{
    if (currentTris <= 0 || targetTris >= currentTris) return 0.0;
    if (targetTris <= 0) return kMaxReduction; // fully decimate
    return clampReduction(1.0 - static_cast<double>(targetTris) / currentTris);
}

double MeshDecimator::reductionFromTargetVerts(int currentVerts, int targetVerts)
{
    // Decimation isn't truly proportional between tris and verts — but for
    // shoulder-of-the-curve assets they track closely enough that targeting
    // a vertex budget gives a useful upper bound on the resulting tri count.
    if (currentVerts <= 0 || targetVerts >= currentVerts) return 0.0;
    if (targetVerts <= 0) return kMaxReduction;
    return clampReduction(1.0 - static_cast<double>(targetVerts) / currentVerts);
}

double MeshDecimator::clampReduction(double r)
{
    if (std::isnan(r) || r <= 0.0) return 0.0;
    if (r > kMaxReduction) return kMaxReduction;
    return r;
}

// LCOV_EXCL_START — exercised via CLI / MCP integration paths
void MeshDecimator::countBaseline(const Ogre::Entity* entity,
                                  int& outTris, int& outVerts)
{
    outTris = 0;
    outVerts = 0;
    if (!entity) return;
    const Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return;
    for (unsigned int s = 0; s < mesh->getNumSubMeshes(); ++s) {
        const Ogre::SubMesh* sub = mesh->getSubMesh(s);
        if (!sub) continue;
        if (sub->indexData)
            outTris += static_cast<int>(sub->indexData->indexCount / 3);
        if (sub->vertexData)
            outVerts += static_cast<int>(sub->vertexData->vertexCount);
    }
    if (mesh->sharedVertexData)
        outVerts += static_cast<int>(mesh->sharedVertexData->vertexCount);
}
// LCOV_EXCL_STOP

// ----- Helpers --------------------------------------------------------------

namespace {

// Ogre::MeshLodGenerator is a Singleton<> — only one instance may exist per
// process. The GUI's MeshLodController owns one; the CLI / MCP / tests don't.
// This helper hands back the live instance, lazily constructing the singleton
// the first time it's needed. The leaked instance is reclaimed by Ogre's
// teardown when the process exits.
Ogre::MeshLodGenerator& sharedLodGenerator()
{
    if (Ogre::MeshLodGenerator* p = Ogre::MeshLodGenerator::getSingletonPtr())
        return *p;
    // The singleton self-registers in its constructor; intentional leak —
    // matches Ogre's standard "singleton lives for the process lifetime"
    // expectation.
    return *(new Ogre::MeshLodGenerator());
}



int countTrianglesInMesh(const Ogre::Mesh* mesh)
{
    if (!mesh) return 0;
    int total = 0;
    for (unsigned int s = 0; s < mesh->getNumSubMeshes(); ++s) {
        const Ogre::SubMesh* sub = mesh->getSubMesh(s);
        if (sub && sub->indexData)
            total += static_cast<int>(sub->indexData->indexCount / 3);
    }
    return total;
}

void collectSubmeshTriCounts(const Ogre::Mesh* mesh,
                             QList<DecimationSubmeshReport>& outSubmeshes,
                             const QString& meshName)
{
    if (!mesh) return;
    for (unsigned int s = 0; s < mesh->getNumSubMeshes(); ++s) {
        const Ogre::SubMesh* sub = mesh->getSubMesh(s);
        if (!sub || !sub->indexData) continue;
        DecimationSubmeshReport sr;
        sr.meshName = meshName;
        sr.submeshIndex = static_cast<int>(s);
        sr.trianglesBefore = static_cast<int>(sub->indexData->indexCount / 3);
        sr.trianglesAfter = sr.trianglesBefore;
        outSubmeshes.append(sr);
    }
}

} // namespace

// ----- Ogre-backed paths ----------------------------------------------------

// LCOV_EXCL_START — Ogre-only path, exercised via manual / CLI tests
DecimationReport MeshDecimator::projectEntity(const Ogre::Entity* entity, double reduction)
{
    DecimationReport report;
    if (!entity) return report;
    const Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return report;

    report.meshName = QString::fromStdString(mesh->getName());
    report.appliedReduction = clampReduction(reduction);

    collectSubmeshTriCounts(mesh.get(), report.submeshes, report.meshName);
    for (auto& sr : report.submeshes) {
        // Predicted after = before * (1 - reduction), rounded. Cap at 1 only
        // when the submesh has triangles to begin with — empty submeshes stay
        // at 0 in the projection (we don't invent triangles).
        if (sr.trianglesBefore <= 0) {
            sr.trianglesAfter = 0;
        } else {
            const double predicted = std::round(sr.trianglesBefore
                                                * (1.0 - report.appliedReduction));
            sr.trianglesAfter = std::max(1, static_cast<int>(predicted));
        }
        report.totalTrianglesBefore += sr.trianglesBefore;
        report.totalTrianglesAfter  += sr.trianglesAfter;
    }
    return report;
}

DecimationReport MeshDecimator::decimateEntity(Ogre::Entity* entity, double reduction)
{
    DecimationReport report;
    if (!entity) return report;
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return report;

    report.meshName = QString::fromStdString(mesh->getName());
    report.appliedReduction = clampReduction(reduction);

    collectSubmeshTriCounts(mesh.get(), report.submeshes, report.meshName);
    for (const auto& sr : report.submeshes)
        report.totalTrianglesBefore += sr.trianglesBefore;

    if (report.appliedReduction <= 0.0) {
        // No-op — caller asked for 0% reduction; just return the baseline.
        report.totalTrianglesAfter = report.totalTrianglesBefore;
        return report;
    }

    // Wipe any existing LOD chain — the base mesh itself is being reduced,
    // so old LODs would point at stale index data after the generator runs.
    mesh->removeLodLevels();

    Ogre::LodConfig lodConfig(mesh);
    lodConfig.createGeneratedLodLevel(0.0f, // distance — meaningless when collapsing in place
                                      static_cast<float>(report.appliedReduction),
                                      Ogre::LodLevel::VRM_PROPORTIONAL);

    try {
        // Use the shared singleton — lazy-construct if no MeshLodController
        // has been instantiated yet (CLI / MCP / test contexts).
        sharedLodGenerator().generateLodLevels(lodConfig);
    } catch (const Ogre::Exception& /*e*/) {
        // Keep totals consistent on failure: per-submesh trianglesAfter
        // already mirrors trianglesBefore (from collectSubmeshTriCounts),
        // so the total should match. applied stays false.
        report.totalTrianglesAfter = report.totalTrianglesBefore;
        return report;
    }

    // Promote LOD 1 (the reduced level) into the base mesh by swapping its
    // index data with the original submesh index data. This is the same
    // pattern MeshLodController::doExportLods uses for per-LOD export, but
    // we keep the swap permanent — the original index buffers are released
    // when the SubMesh destructor runs at scene teardown.
    for (unsigned int s = 0; s < mesh->getNumSubMeshes(); ++s) {
        Ogre::SubMesh* sub = mesh->getSubMesh(s);
        if (!sub) continue;
        if (sub->mLodFaceList.empty() || !sub->mLodFaceList.front()) continue;
        Ogre::IndexData* original = sub->indexData;
        sub->indexData = sub->mLodFaceList.front();
        sub->mLodFaceList.front() = original; // keep ownership balanced
    }
    mesh->removeLodLevels();

    // Re-count post-decimation triangles per submesh.
    int subIdx = 0;
    for (unsigned int s = 0; s < mesh->getNumSubMeshes(); ++s) {
        const Ogre::SubMesh* sub = mesh->getSubMesh(s);
        if (!sub || !sub->indexData) continue;
        if (subIdx < report.submeshes.size())
            report.submeshes[subIdx].trianglesAfter =
                static_cast<int>(sub->indexData->indexCount / 3);
        ++subIdx;
    }
    report.totalTrianglesAfter = countTrianglesInMesh(mesh.get());
    report.applied = true;
    return report;
}
// LCOV_EXCL_STOP

// ----- Serialisation --------------------------------------------------------

QJsonObject MeshDecimator::toJson(const DecimationReport& report)
{
    QJsonObject obj;
    obj["mesh"] = report.meshName;
    obj["appliedReduction"] = report.appliedReduction;
    obj["applied"] = report.applied;

    QJsonArray submeshes;
    for (const auto& sr : report.submeshes) {
        QJsonObject so;
        so["submeshIndex"] = sr.submeshIndex;
        so["trianglesBefore"] = sr.trianglesBefore;
        so["trianglesAfter"] = sr.trianglesAfter;
        submeshes.append(so);
    }
    obj["submeshes"] = submeshes;

    QJsonObject totals;
    totals["trianglesBefore"] = report.totalTrianglesBefore;
    totals["trianglesAfter"] = report.totalTrianglesAfter;
    totals["effectiveReduction"] = report.effectiveReduction();
    obj["totals"] = totals;
    return obj;
}

QString MeshDecimator::toText(const DecimationReport& report)
{
    QString out;
    QTextStream s(&out);
    QLocale locale;

    s << "Mesh Decimation\n";
    s << "===============\n\n";

    if (report.meshName.isEmpty()) {
        s << "(no mesh)\n";
        return out;
    }

    s << "Mesh: " << report.meshName << "\n";
    s << "Reduction requested: "
      << QString::number(report.appliedReduction * 100.0, 'f', 1) << "%"
      << (report.applied ? "  (applied)" : "  (projected)")
      << "\n\n";

    for (const auto& sr : report.submeshes) {
        s << "  [" << sr.submeshIndex << "] tris "
          << locale.toString(sr.trianglesBefore) << " → "
          << locale.toString(sr.trianglesAfter) << "\n";
    }

    s << "\nTotal: "
      << locale.toString(report.totalTrianglesBefore) << " → "
      << locale.toString(report.totalTrianglesAfter)
      << " (" << QString::number(report.effectiveReduction() * 100.0, 'f', 1)
      << "% effective reduction)\n";
    return out;
}
