#include "MeshValidator.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "MeshImporterExporter.h"
#include "SentryReporter.h"
#include "DrawCallAnalyzer.h"
#include "MemoryEstimator.h"
#include "VertexCacheOptimizer.h"
#include <Ogre.h>
#include <assimp/postprocess.h>
#include <cmath>
#include <QDir>
#include <QLocale>
#include <QTemporaryDir>

namespace {

QList<Ogre::Entity*> validationTargetEntities()
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel)
        return {};
    return sel->getResolvedEntities();
}

} // namespace

MeshValidator* MeshValidator::m_pSingleton = nullptr;

MeshValidator* MeshValidator::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new MeshValidator();
    return m_pSingleton;
}

MeshValidator* MeshValidator::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine);
    Q_UNUSED(scriptEngine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void MeshValidator::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

MeshValidator::MeshValidator() : QObject(nullptr)
{
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, [this]() {
        // Clear stale results when selection changes; cancel any pending validation.
        m_issues.clear();
        m_validated = false;
        if (m_pendingValidate) {
            m_pendingValidate = false;
            emit validatingChanged();
        }
        emit selectionChanged();
        emit issuesChanged();
    });
}

MeshValidator::~MeshValidator()
{
    if (m_registeredRoot)
        m_registeredRoot->removeFrameListener(this);
}

bool MeshValidator::hasSelection() const
{
    return !validationTargetEntities().isEmpty();
}

bool MeshValidator::hasFixableIssues() const
{
    for (const QVariant& v : m_issues) {
        if (v.toMap().value("fixable").toBool())
            return true;
    }
    return false;
}

// ---- helpers ----

static Ogre::Vector3 getPosition(const unsigned char* vertexBase, size_t stride,
                                  const Ogre::VertexElement* elem, size_t idx)
{
    const unsigned char* ptr = vertexBase + idx * stride;
    float* pf = nullptr;
    elem->baseVertexPointerToElement(const_cast<unsigned char*>(ptr), &pf);
    return Ogre::Vector3(pf[0], pf[1], pf[2]);
}

static void getTexCoord(const unsigned char* vertexBase, size_t stride,
                         const Ogre::VertexElement* elem, size_t idx,
                         float& u, float& v)
{
    const unsigned char* ptr = vertexBase + idx * stride;
    float* pf = nullptr;
    elem->baseVertexPointerToElement(const_cast<unsigned char*>(ptr), &pf);
    u = pf[0];
    v = pf[1];
}

void MeshValidator::validate()
{
    m_issues.clear();
    m_validated = false;
    emit issuesChanged();

    if (validationTargetEntities().isEmpty())
        return;

    SentryReporter::addBreadcrumb("ui.action", "Validate mesh");

    // On Linux/GL3Plus, glMapBufferRange requires an active OpenGL context.
    // Deferring to frameStarted() guarantees the Ogre context is current.
    // Re-register on the current Root each time it may have changed (e.g. after
    // Manager::kill() / re-init in test flows or in-process restarts).
    if (auto* mgr = Manager::getSingletonPtr()) {
        if (auto* root = mgr->getRoot()) {
            if (m_registeredRoot != root) {
                if (m_registeredRoot)
                    m_registeredRoot->removeFrameListener(this);
                root->addFrameListener(this);
                m_registeredRoot = root;
            }
        }
    }
    m_pendingValidate = true;
    emit validatingChanged();
}

bool MeshValidator::frameStarted(const Ogre::FrameEvent& /*evt*/)
{
    if (m_pendingValidate) {
        m_pendingValidate = false;
        emit validatingChanged();
        doValidate();
    }
    return true;
}

void MeshValidator::doValidate()
{
    m_issues.clear();
    m_validated = false;
    m_cacheOptimizationAvailable = false;

    const QList<Ogre::Entity*> targets = validationTargetEntities();
    if (targets.isEmpty()) {
        emit issuesChanged();
        return;
    }

    int totalDegenerates = 0;
    int totalNonFiniteUV = 0;
    int totalOutOfRangeUV = 0;
    int totalTris = 0;
    int totalVerts = 0;
    int totalSubmeshes = 0;
    int meshesWithUVs = 0;
    int meshesWithoutUVs = 0;

    for (Ogre::Entity* entity : targets) {
        Ogre::MeshPtr mesh = entity->getMesh();
        if (!mesh) continue;

        for (unsigned si = 0; si < mesh->getNumSubMeshes(); ++si) {
            Ogre::SubMesh* sub = mesh->getSubMesh(si);
            Ogre::VertexData* vd = sub->useSharedVertices ? mesh->sharedVertexData : sub->vertexData;
            Ogre::IndexData* id = sub->indexData;
            if (!vd || !id || !id->indexBuffer) continue;

            ++totalSubmeshes;
            totalVerts += static_cast<int>(vd->vertexCount);
            totalTris  += static_cast<int>(id->indexCount / 3);

            const Ogre::VertexElement* posElem =
                vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
            const Ogre::VertexElement* texElem =
                vd->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES);

            if (texElem) ++meshesWithUVs;
            else         ++meshesWithoutUVs;

            // ---- lock position buffer ----
            Ogre::HardwareVertexBufferSharedPtr vbuf;
            const unsigned char* vdata = nullptr;
            size_t vStride = 0;

            if (posElem) {
                vbuf = vd->vertexBufferBinding->getBuffer(posElem->getSource());
                vdata = static_cast<const unsigned char*>(
                    vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
                vStride = vbuf->getVertexSize();
            }

            // ---- check UV non-finite / out-of-range ----
            // Positions and UVs are often interleaved in the same hardware buffer.
            // Locking the same buffer twice throws "already locked" — reuse the
            // already-locked pointer when both elements share the same source index.
            if (texElem) {
                bool sharedBuf = posElem && (texElem->getSource() == posElem->getSource());
                Ogre::HardwareVertexBufferSharedPtr tbuf;
                const unsigned char* tdata = nullptr;
                size_t tStride = 0;

                if (sharedBuf) {
                    tdata   = vdata;
                    tStride = vStride;
                } else {
                    tbuf    = vd->vertexBufferBinding->getBuffer(texElem->getSource());
                    tdata   = static_cast<const unsigned char*>(
                        tbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
                    tStride = tbuf->getVertexSize();
                }

                for (size_t vi = 0; vi < vd->vertexCount; ++vi) {
                    float u = 0, v = 0;
                    getTexCoord(tdata, tStride, texElem, vi, u, v);
                    if (!std::isfinite(u) || !std::isfinite(v))
                        ++totalNonFiniteUV;
                    else if (u < -10.f || u > 10.f || v < -10.f || v > 10.f)
                        ++totalOutOfRangeUV;
                }

                if (!sharedBuf && tbuf)
                    tbuf->unlock();
            }

            // ---- lock index buffer + check degenerate triangles ----
            if (posElem && vdata && id->indexCount >= 3) {
                bool use16 = (id->indexBuffer->getType() == Ogre::HardwareIndexBuffer::IT_16BIT);
                const void* idata = id->indexBuffer->lock(Ogre::HardwareBuffer::HBL_READ_ONLY);
                const auto* idx16 = static_cast<const uint16_t*>(idata);
                const auto* idx32 = static_cast<const uint32_t*>(idata);

                for (size_t ti = 0; ti + 2 < id->indexCount; ti += 3) {
                    size_t i0, i1, i2;
                    if (use16) {
                        i0 = idx16[id->indexStart + ti];
                        i1 = idx16[id->indexStart + ti + 1];
                        i2 = idx16[id->indexStart + ti + 2];
                    } else {
                        i0 = idx32[id->indexStart + ti];
                        i1 = idx32[id->indexStart + ti + 1];
                        i2 = idx32[id->indexStart + ti + 2];
                    }

                    if (i0 >= vd->vertexCount || i1 >= vd->vertexCount || i2 >= vd->vertexCount)
                        continue;

                    Ogre::Vector3 p0 = getPosition(vdata, vStride, posElem, i0);
                    Ogre::Vector3 p1 = getPosition(vdata, vStride, posElem, i1);
                    Ogre::Vector3 p2 = getPosition(vdata, vStride, posElem, i2);
                    float area = (p1 - p0).crossProduct(p2 - p0).length();
                    if (area < 1e-6f)
                        ++totalDegenerates;
                }

                id->indexBuffer->unlock();
            }

            if (vbuf)
                vbuf->unlock();
        }
    }

    // ---- build the checklist ----
    //
    // Every checked dimension produces a row: errors/warnings when something is
    // wrong, an "ok" row when the check passed, an "info" row for neutral
    // observations (draw calls / memory — neither pass nor fail, just data).
    // This way the user always sees what was actually analyzed rather than a
    // bare "No issues found." that hides the scope of the validation.
    QLocale locale;

    // 1. Geometry — degenerate triangles
    if (totalDegenerates > 0) {
        QVariantMap issue;
        issue["type"] = "error";
        issue["description"] = QString("Geometry: %1 degenerate triangle(s) — zero-area faces")
                                   .arg(totalDegenerates);
        issue["count"] = totalDegenerates;
        issue["fixable"] = true;
        m_issues.append(issue);
    } else {
        QVariantMap issue;
        issue["type"] = "ok";
        issue["description"] = QString("Geometry: %1 triangle(s) across %2 submesh(es), no degenerate faces")
                                   .arg(locale.toString(totalTris))
                                   .arg(totalSubmeshes);
        issue["count"] = 0;
        issue["fixable"] = false;
        m_issues.append(issue);
    }

    // 2. UVs — finite / range. Skip the check entirely when the mesh has no UVs.
    if (meshesWithUVs == 0 && meshesWithoutUVs > 0) {
        QVariantMap issue;
        issue["type"] = "info";
        issue["description"] = QStringLiteral("UVs: no texture coordinates on this mesh — skipped");
        issue["count"] = 0;
        issue["fixable"] = false;
        m_issues.append(issue);
    } else {
        if (totalNonFiniteUV > 0) {
            QVariantMap issue;
            issue["type"] = "error";
            issue["description"] = QString("UVs: %1 vertex(es) with non-finite coordinates (NaN/Inf)")
                                       .arg(totalNonFiniteUV);
            issue["count"] = totalNonFiniteUV;
            issue["fixable"] = true;
            m_issues.append(issue);
        }
        if (totalOutOfRangeUV > 0) {
            QVariantMap issue;
            issue["type"] = "warning";
            issue["description"] = QString("UVs: %1 vertex(es) with extreme values (outside ±10)")
                                       .arg(totalOutOfRangeUV);
            issue["count"] = totalOutOfRangeUV;
            issue["fixable"] = false;
            m_issues.append(issue);
        }
        if (totalNonFiniteUV == 0 && totalOutOfRangeUV == 0) {
            QVariantMap issue;
            issue["type"] = "ok";
            issue["description"] = QStringLiteral("UVs: all finite, all within ±10 range");
            issue["count"] = 0;
            issue["fixable"] = false;
            m_issues.append(issue);
        }
    }

    // 3. Draw-call analysis (Phase 6 slice B). Neutral observation: flag merge
    // opportunities as "info" so they show up in the report without looking
    // like a failure.
    const DrawCallReport drawReport = DrawCallAnalyzer::analyze(targets);
    if (drawReport.totalDrawCalls > 0) {
        QVariantMap issue;
        issue["count"] = drawReport.totalDrawCalls;
        issue["fixable"] = false;
        if (drawReport.totalSavings > 0) {
            issue["type"] = "info";
            issue["description"] = QString("Draws: %1 across %2 material(s) — save %3 by merging "
                                           "entities that share a material")
                                       .arg(drawReport.totalDrawCalls)
                                       .arg(drawReport.uniqueMaterials)
                                       .arg(drawReport.totalSavings);
        } else {
            issue["type"] = "ok";
            issue["description"] = QString("Draws: %1 across %2 material(s) — no merge opportunities")
                                       .arg(drawReport.totalDrawCalls)
                                       .arg(drawReport.uniqueMaterials);
        }
        m_issues.append(issue);
    }

    // 4. Vertex cache ACMR (Phase 6 slice C). Pure analysis — never rewrites
    // the index buffer from validation. The qtmesh CLI / MCP / future inspector
    // button does the actual reorder when the user opts in.
    VertexCacheReport cacheReport;
    for (Ogre::Entity* entity : targets) {
        const VertexCacheReport partial =
            VertexCacheOptimizer::analyzeEntity(entity, /*rewrite=*/false);
        for (const SubMeshCacheReport& sr : partial.submeshes) {
            cacheReport.submeshes.append(sr);
            cacheReport.totalTriangles += sr.triangleCount;
            cacheReport.weightedAcmrBefore += sr.acmrBefore * sr.triangleCount;
            cacheReport.weightedAcmrAfter  += sr.acmrAfter  * sr.triangleCount;
        }
    }
    if (cacheReport.totalTriangles > 0) {
        cacheReport.weightedAcmrBefore /= cacheReport.totalTriangles;
        cacheReport.weightedAcmrAfter  /= cacheReport.totalTriangles;

        const double improvementPct = cacheReport.improvement();
        const bool meaningfulGain = improvementPct >= 1.0;
        // Round to 1 dp to avoid surfacing 0.4% as a "fix me" call to action.

        QVariantMap issue;
        if (meaningfulGain) {
            m_cacheOptimizationAvailable = true;
            issue["type"] = "info";
            issue["description"] =
                QString("Vertex cache: ACMR %1 → %2 (%3% improvement available — "
                        "click \"Optimize Vertex Cache\" below)")
                    .arg(QString::number(cacheReport.weightedAcmrBefore, 'f', 3),
                         QString::number(cacheReport.weightedAcmrAfter,  'f', 3),
                         QString::number(improvementPct, 'f', 1));
        } else {
            issue["type"] = "ok";
            issue["description"] = QString("Vertex cache: ACMR %1 — already optimal "
                                           "(no meaningful gain from reordering)")
                                       .arg(QString::number(cacheReport.weightedAcmrBefore, 'f', 3));
        }
        issue["count"] = 0;
        issue["fixable"] = false;
        m_issues.append(issue);
    }

    // 5. Memory / VRAM (Phase 6 slice A). Always info — no pass/fail without
    // a configured budget; we just report what the asset costs on the GPU.
    quint64 meshBytes = 0;
    for (Ogre::Entity* entity : targets) {
        const MeshMemoryEstimate est = MemoryEstimator::estimateEntity(entity);
        meshBytes += est.totalBytes();
    }
    if (meshBytes > 0) {
        QVariantMap issue;
        issue["type"] = "info";
        issue["description"] = QString("GPU: ~%1 of vertex + index buffers (%2 vert / %3 tri)")
                                   .arg(MemoryEstimator::formatBytes(meshBytes))
                                   .arg(locale.toString(totalVerts))
                                   .arg(locale.toString(totalTris));
        issue["count"] = 0;
        issue["fixable"] = false;
        m_issues.append(issue);
    }

    m_validated = true;
    emit issuesChanged();
}

void MeshValidator::fixAll()
{
    const QList<Ogre::Entity*> targets = validationTargetEntities();
    if (targets.isEmpty()) {
        emit error("No mesh selected.");
        return;
    }

    SentryReporter::addBreadcrumb("ui.action", "Fix mesh issues (re-import with cleanup)");

    // Export each selected entity to a temp file then reimport with cleanup flags.
    // This creates a new cleaned entity; the user can delete the original.
    const unsigned int cleanFlags = aiProcess_FindDegenerates
                                  | aiProcess_FindInvalidData
                                  | aiProcess_SortByPType;

    // Export to a temp OBJ file so Assimp processes the cleanup flags.
    // .mesh files use Ogre's native loader and bypass Assimp entirely.
    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        emit error("Could not create temporary directory for cleaning.");
        return;
    }

    QStringList reimportPaths;
    for (Ogre::Entity* entity : targets) {
        Ogre::SceneNode* sn = entity->getParentSceneNode();
        if (!sn) continue;

        const QString nodeName = QString::fromStdString(sn->getName());
        const QString tmpPath = QDir(tmpDir.path()).filePath(nodeName + "_clean.obj");
        if (MeshImporterExporter::exporter(sn, tmpPath, "obj", /*stripAnimations=*/false) == 0)
            reimportPaths << tmpPath;
    }

    if (reimportPaths.isEmpty()) {
        emit error("Export failed — could not prepare mesh for cleaning.");
        return;
    }

    MeshImporterExporter::importer(reimportPaths, cleanFlags);

    emit fixApplied(QString("Cleaned mesh imported. %1 original(s) can now be deleted.")
                    .arg(reimportPaths.size()));

    // Re-validate the newly imported entities
    validate();
}

void MeshValidator::optimizeVertexCache()
{
    const QList<Ogre::Entity*> targets = validationTargetEntities();
    if (targets.isEmpty()) {
        emit error("No mesh selected.");
        return;
    }

    SentryReporter::addBreadcrumb("ui.action", "Optimize vertex cache (Forsyth, in place)");

    VertexCacheReport aggregate;
    for (Ogre::Entity* entity : targets) {
        const VertexCacheReport partial =
            VertexCacheOptimizer::analyzeEntity(entity, /*rewrite=*/true);
        for (const SubMeshCacheReport& sr : partial.submeshes) {
            aggregate.submeshes.append(sr);
            aggregate.totalTriangles += sr.triangleCount;
            aggregate.weightedAcmrBefore += sr.acmrBefore * sr.triangleCount;
            aggregate.weightedAcmrAfter  += sr.acmrAfter  * sr.triangleCount;
            if (sr.reordered) ++aggregate.totalReordered;
        }
    }
    if (aggregate.totalTriangles > 0) {
        aggregate.weightedAcmrBefore /= aggregate.totalTriangles;
        aggregate.weightedAcmrAfter  /= aggregate.totalTriangles;
    }

    if (aggregate.totalReordered == 0) {
        emit fixApplied("Vertex cache was already optimal — no submeshes were reordered.");
    } else {
        emit fixApplied(QString("Reordered %1 submesh(es). ACMR %2 → %3 (%4% improvement).")
                            .arg(aggregate.totalReordered)
                            .arg(QString::number(aggregate.weightedAcmrBefore, 'f', 3),
                                 QString::number(aggregate.weightedAcmrAfter,  'f', 3),
                                 QString::number(aggregate.improvement(), 'f', 1)));
    }

    // Refresh the checklist — the row should flip to "already optimal".
    validate();
}
