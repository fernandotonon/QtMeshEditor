#include "MeshValidator.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "MeshImporterExporter.h"
#include "SentryReporter.h"
#include <Ogre.h>
#include <assimp/postprocess.h>
#include <cmath>
#include <QDir>
#include <QTemporaryDir>

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
    auto* sel = SelectionSet::getSingleton();
    return sel && sel->hasEntities();
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

    auto* sel = SelectionSet::getSingleton();
    if (!sel || !sel->hasEntities())
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

    auto* sel = SelectionSet::getSingleton();
    if (!sel || !sel->hasEntities()) {
        emit issuesChanged();
        return;
    }

    int totalDegenerates = 0;
    int totalNonFiniteUV = 0;
    int totalOutOfRangeUV = 0;

    for (Ogre::Entity* entity : sel->getEntitiesSelectionList()) {
        Ogre::MeshPtr mesh = entity->getMesh();
        if (!mesh) continue;

        for (unsigned si = 0; si < mesh->getNumSubMeshes(); ++si) {
            Ogre::SubMesh* sub = mesh->getSubMesh(si);
            Ogre::VertexData* vd = sub->useSharedVertices ? mesh->sharedVertexData : sub->vertexData;
            Ogre::IndexData* id = sub->indexData;
            if (!vd || !id || !id->indexBuffer) continue;

            const Ogre::VertexElement* posElem =
                vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
            const Ogre::VertexElement* texElem =
                vd->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES);

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
                    else if (u < -10.f || u > 11.f || v < -10.f || v > 11.f)
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

    // ---- build issues list ----
    if (totalDegenerates > 0) {
        QVariantMap issue;
        issue["type"] = "error";
        issue["description"] = QString("%1 degenerate triangle(s) — zero-area faces").arg(totalDegenerates);
        issue["count"] = totalDegenerates;
        issue["fixable"] = true;
        m_issues.append(issue);
    }
    if (totalNonFiniteUV > 0) {
        QVariantMap issue;
        issue["type"] = "error";
        issue["description"] = QString("%1 vertex(es) with non-finite UV coordinates (NaN/Inf)").arg(totalNonFiniteUV);
        issue["count"] = totalNonFiniteUV;
        issue["fixable"] = true;
        m_issues.append(issue);
    }
    if (totalOutOfRangeUV > 0) {
        QVariantMap issue;
        issue["type"] = "warning";
        issue["description"] = QString("%1 vertex(es) with extreme UV values (outside ±10)").arg(totalOutOfRangeUV);
        issue["count"] = totalOutOfRangeUV;
        issue["fixable"] = false;
        m_issues.append(issue);
    }

    if (m_issues.isEmpty()) {
        QVariantMap ok;
        ok["type"] = "ok";
        ok["description"] = "No issues found.";
        ok["count"] = 0;
        ok["fixable"] = false;
        m_issues.append(ok);
    }

    m_validated = true;
    emit issuesChanged();
}

void MeshValidator::fixAll()
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel || !sel->hasEntities()) {
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
    for (Ogre::Entity* entity : sel->getEntitiesSelectionList()) {
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
