#include "UVEditorController.h"

#include "EditableMesh.h"
#include "EditModeController.h"
#include "HalfEdgeMesh.h"
#include "EmbeddedTextureCache.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <QImage>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <OgreEntity.h>
#include <OgreMaterial.h>
#include <OgrePass.h>
#include <OgreSubEntity.h>
#include <OgreSubMesh.h>
#include <OgreTexture.h>
#include <OgreTextureManager.h>
#include <OgreTextureUnitState.h>

#include <queue>
#include <cmath>
#include <algorithm>

UVEditorController* UVEditorController::s_instance = nullptr;

UVEditorController* UVEditorController::instance()
{
    if (!s_instance)
        s_instance = new UVEditorController();
    return s_instance;
}

UVEditorController* UVEditorController::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    Q_UNUSED(engine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void UVEditorController::kill()
{
    delete s_instance;
    s_instance = nullptr;
}

UVEditorController::UVEditorController(QObject* parent)
    : QObject(parent)
{
    connectSignals();
    // Starts inactive + dirty: the first setActive(true) (UV dock shown) builds
    // the cache. No eager rebuild — nothing is watching the panel yet.
}

void UVEditorController::connectSignals()
{
    // Signal-driven refreshes go through onSourceChanged(), which is GATED on
    // the panel being active (visible) — so import-time signal storms don't
    // rebuild the UV cache when nobody's looking. Explicit refresh() still
    // rebuilds immediately.
    if (auto* sel = SelectionSet::getSingleton()) {
        connect(sel, &SelectionSet::selectionChanged, this, &UVEditorController::onSourceChanged);
    }
    if (auto* mgr = Manager::getSingletonPtr()) {
        connect(mgr, &Manager::entityCreated, this, &UVEditorController::onSourceChanged);
        connect(mgr, &Manager::sceneNodeDestroyed, this, &UVEditorController::onSourceChanged);
    }
    if (auto* edit = EditModeController::instance()) {
        connect(edit, &EditModeController::meshDataChanged, this, &UVEditorController::onSourceChanged);
        connect(edit, &EditModeController::editModeChanged, this, &UVEditorController::onSourceChanged);
    }
}

void UVEditorController::setUvChannel(int channel)
{
    channel = std::max(0, std::min(channel, 1));
    if (m_uvChannel == channel)
        return;
    m_uvChannel = channel;
    emit uvChannelChanged();
    rebuildMeshCache();
}

void UVEditorController::setShowTextureBackground(bool on)
{
    if (m_showTextureBackground == on)
        return;
    m_showTextureBackground = on;
    emit showTextureBackgroundChanged();
}

void UVEditorController::refresh()
{
    // Explicit refresh (Q_INVOKABLE — a deliberate QML/test/user action) always
    // rebuilds, regardless of the active gate.
    m_dirty = false;
    rebuildMeshCache();
}

void UVEditorController::onSourceChanged()
{
    // Signal-driven auto-refresh (selection / entityCreated / mesh edits). Only
    // do the expensive rebuild when the panel is active (visible); otherwise
    // just mark the cache stale so setActive(true) rebuilds it lazily. This is
    // what keeps import — which fires entityCreated + selectionChanged
    // repeatedly — from paying per-entity UV reads + island computation while
    // the UV editor is closed (the common case, and the reported slowdown).
    if (!m_active) {
        m_dirty = true;
        return;
    }
    rebuildMeshCache();
}

void UVEditorController::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (m_active && m_dirty) {
        m_dirty = false;
        rebuildMeshCache();
    }
}

bool UVEditorController::readUvChannel(const Ogre::VertexData* vertexData, int channel,
                                      std::vector<Ogre::Vector2>& outUvs)
{
    outUvs.clear();
    if (!vertexData || vertexData->vertexCount == 0)
        return false;

    const auto* elem = vertexData->vertexDeclaration->findElementBySemantic(
        Ogre::VES_TEXTURE_COORDINATES, static_cast<unsigned short>(channel));
    if (!elem)
        return false;

    outUvs.resize(vertexData->vertexCount, Ogre::Vector2::ZERO);
    auto vbuf = vertexData->vertexBufferBinding->getBuffer(elem->getSource());
    if (!vbuf)
        return false;

    const size_t stride = vbuf->getVertexSize();
    auto* base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    for (size_t i = 0; i < vertexData->vertexCount; ++i) {
        float* p = nullptr;
        elem->baseVertexPointerToElement(base + i * stride, &p);
        outUvs[i] = Ogre::Vector2(p[0], p[1]);
    }
    vbuf->unlock();
    return true;
}

void UVEditorController::applyUvChannel(EditableMesh& mesh, Ogre::Entity* entity, int channel,
                                        const QSet<int>& submeshFilter)
{
    if (channel == 0 || !entity || !entity->getMesh())
        return;

    Ogre::Mesh* ogreMesh = entity->getMesh().get();
    for (size_t si = 0; si < mesh.subMeshes().size(); ++si) {
        if (!submeshFilter.isEmpty() && !submeshFilter.contains(static_cast<int>(si)))
            continue;
        if (si >= ogreMesh->getNumSubMeshes())
            continue;

        const Ogre::SubMesh* sub = ogreMesh->getSubMesh(static_cast<unsigned short>(si));
        const Ogre::VertexData* vd = sub->useSharedVertices ? ogreMesh->sharedVertexData : sub->vertexData;
        std::vector<Ogre::Vector2> uvs;
        auto& verts = mesh.subMeshes()[si].vertices;
        if (!readUvChannel(vd, channel, uvs)) {
            for (auto& v : verts)
                v.hasUV = false;
            continue;
        }

        const size_t n = std::min(verts.size(), uvs.size());
        for (size_t vi = 0; vi < n; ++vi) {
            verts[vi].uv = uvs[vi];
            verts[vi].hasUV = true;
        }
    }
}

QString UVEditorController::colorForIsland(int islandId)
{
    const int hue = (islandId * 67) % 360;
    return QColor::fromHsv(hue, 170, 220, 200).name(QColor::HexArgb);
}

UVEditorController::IslandResult UVEditorController::computeIslandsFromHalfEdgeMesh(const HalfEdgeMesh& hem)
{
    IslandResult result;
    const int faceTotal = static_cast<int>(hem.faceCount());
    result.faceIslandIds.assign(faceTotal, -1);

    for (int start = 0; start < faceTotal; ++start) {
        if (result.faceIslandIds[start] >= 0)
            continue;

        std::queue<int> q;
        q.push(start);
        result.faceIslandIds[start] = result.islandCount;

        while (!q.empty()) {
            const int faceIdx = q.front();
            q.pop();

            const int startHE = hem.face(faceIdx).halfEdge;
            if (startHE < 0)
                continue;

            int he = startHE;
            do {
                const int twinIdx = hem.halfEdge(he).twin;
                if (twinIdx >= 0) {
                    const int adjFace = hem.halfEdge(twinIdx).face;
                    if (adjFace >= 0 && result.faceIslandIds[adjFace] < 0) {
                        result.faceIslandIds[adjFace] = result.islandCount;
                        q.push(adjFace);
                    }
                }
                he = hem.halfEdge(he).next;
            } while (he != startHE);
        }

        ++result.islandCount;
    }

    return result;
}

UVEditorController::IslandResult UVEditorController::computeIslandsFromEditableMesh(const EditableMesh& mesh)
{
    if (mesh.subMeshes().empty())
        return {};

    HalfEdgeMesh hem;
    if (!hem.buildFromEditableMesh(mesh))
        return {};

    return computeIslandsFromHalfEdgeMesh(hem);
}

namespace {

QString safePreviewBaseName(const QString& texName)
{
    QString base = QFileInfo(texName).fileName();
    base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")),
                 QStringLiteral("_"));
    if (base.isEmpty())
        base = QStringLiteral("texture");
    return base;
}

#ifdef Q_OS_MACOS
QString macAppBundleRoot()
{
    const QString bundleRoot =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../.."));
    const QString canonical = QFileInfo(bundleRoot).canonicalFilePath();
    return canonical.isEmpty() ? bundleRoot : canonical;
}
#endif

} // namespace

static QString fileUrl(const QString& path)
{
    return QUrl::fromLocalFile(path).toString();
}

static Ogre::TexturePtr findLoadedTextureByName(const std::string& name)
{
    auto texPtr = Ogre::TextureManager::getSingleton().getByName(name);
    if (texPtr)
        return texPtr;
    auto it = Ogre::TextureManager::getSingleton().getResourceIterator();
    while (it.hasMoreElements()) {
        const Ogre::ResourcePtr r = it.getNext();
        if (r && r->getName() == name)
            return Ogre::static_pointer_cast<Ogre::Texture>(r);
    }
    return {};
}

static QString diffuseTextureNameForSubEntity(Ogre::SubEntity* sub)
{
    if (!sub)
        return {};
    const Ogre::MaterialPtr mat = sub->getMaterial();
    if (!mat || mat->getNumTechniques() == 0)
        return {};
    auto* tech = mat->getTechnique(0);
    if (!tech || tech->getNumPasses() == 0)
        return {};
    auto* pass = tech->getPass(0);
    if (!pass)
        return {};

    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        const std::string n = tus->getName();
        if (n == "diffuse_map" || n == "albedo" || n == "DiffuseColor")
            return QString::fromStdString(tus->getTextureName());
    }
    if (pass->getNumTextureUnitStates() > 0)
        return QString::fromStdString(pass->getTextureUnitState(0)->getTextureName());
    return {};
}

QString UVEditorController::resolveDiffuseTextureSource(Ogre::Entity* entity, int submeshIndex)
{
    if (!entity || submeshIndex < 0
        || submeshIndex >= static_cast<int>(entity->getNumSubEntities()))
        return {};

    const QString texName = diffuseTextureNameForSubEntity(entity->getSubEntity(submeshIndex));
    if (texName.isEmpty())
        return {};

    if (auto texPtr = findLoadedTextureByName(texName.toStdString())) {
        const QString origin = QString::fromStdString(texPtr->getOrigin());
        if (!origin.isEmpty() && QFileInfo::exists(origin)) {
            SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                QStringLiteral("UV editor texture from Ogre origin: %1").arg(texName));
            return fileUrl(origin);
        }
    }

    const std::vector<uint8_t> embedded = EmbeddedTextureCache::retrieve(texName.toStdString());
    if (!embedded.empty()) {
        const QString outDir =
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                .filePath(QStringLiteral("uv_editor_previews"));
        QDir().mkpath(outDir);
        const QString outPath =
            QDir(outDir).filePath(safePreviewBaseName(texName) + QStringLiteral("_uvbg.png"));
        if (!QFileInfo::exists(outPath)) {
            QImage img;
            if (img.loadFromData(embedded.data(), static_cast<int>(embedded.size()))) {
                img.save(outPath, "PNG");
                SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
                    QStringLiteral("UV editor embedded preview: %1").arg(texName));
            }
        }
        if (QFileInfo::exists(outPath))
            return fileUrl(outPath);
    }

    QStringList candidates = {
        QStringLiteral("media/materials/textures/%1").arg(texName),
        QDir::current().filePath(texName)
    };
#ifdef Q_OS_MACOS
    candidates.prepend(QDir(macAppBundleRoot()).filePath(
        QStringLiteral("media/materials/textures/%1").arg(texName)));
    candidates.append(QDir(macAppBundleRoot()).filePath(texName));
#else
    candidates.append(QDir(QCoreApplication::applicationDirPath()).filePath(texName));
#endif
    for (const QString& path : candidates) {
        if (QFileInfo::exists(path)) {
            SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                QStringLiteral("UV editor texture from disk: %1").arg(texName));
            return fileUrl(QFileInfo(path).absoluteFilePath());
        }
    }
    return {};
}

static EditableMesh filteredEditableMesh(const EditableMesh& source, const QSet<int>& submeshFilter)
{
    EditableMesh out;
    if (submeshFilter.isEmpty()) {
        out.subMeshes() = source.subMeshes();
        return out;
    }

    for (size_t si = 0; si < source.subMeshes().size(); ++si) {
        if (submeshFilter.contains(static_cast<int>(si)))
            out.subMeshes().push_back(source.subMeshes()[si]);
    }
    return out;
}

bool UVEditorController::buildFromEntity(Ogre::Entity* entity, const QSet<int>& submeshFilter, int uvChannel)
{
    if (!entity)
        return false;

    EditableMesh displayMesh;
    if (auto* edit = EditModeController::instance()) {
        if (edit->isEditModeActive() && edit->editEntity() == entity && edit->currentMesh()) {
            displayMesh.subMeshes() = edit->currentMesh()->subMeshes();
        } else if (!displayMesh.loadFromEntity(entity)) {
            return false;
        }
    } else if (!displayMesh.loadFromEntity(entity)) {
        return false;
    }

    applyUvChannel(displayMesh, entity, uvChannel, submeshFilter);
    EditableMesh mesh = filteredEditableMesh(displayMesh, submeshFilter);
    if (mesh.subMeshes().empty())
        return false;

    HalfEdgeMesh hem;
    if (!hem.buildFromEditableMesh(mesh))
        return false;

    const IslandResult islands = computeIslandsFromHalfEdgeMesh(hem);

    float uMin = 1e9f, vMin = 1e9f, uMax = -1e9f, vMax = -1e9f;
    QVariantList tris;
    tris.reserve(static_cast<int>(hem.faceCount()));

    int heFaceIdx = 0;
    for (const auto& sub : mesh.subMeshes()) {
        auto emitTriangle = [&](const EditableTriangle& tri) {
            if (heFaceIdx >= static_cast<int>(islands.faceIslandIds.size()))
                return;

            Ogre::Vector2 uvs[3];
            bool ok = true;
            for (int c = 0; c < 3; ++c) {
                const size_t vi = tri.indices[c];
                if (vi >= sub.vertices.size() || !sub.vertices[vi].hasUV) {
                    ok = false;
                    break;
                }
                uvs[c] = sub.vertices[vi].uv;
            }
            if (!ok)
                return;

            uMin = std::min({uMin, uvs[0].x, uvs[1].x, uvs[2].x});
            vMin = std::min({vMin, uvs[0].y, uvs[1].y, uvs[2].y});
            uMax = std::max({uMax, uvs[0].x, uvs[1].x, uvs[2].x});
            vMax = std::max({vMax, uvs[0].y, uvs[1].y, uvs[2].y});

            const int islandId = islands.faceIslandIds[heFaceIdx];
            tris.push_back(QVariantMap{
                {QStringLiteral("u0"), uvs[0].x},
                {QStringLiteral("v0"), uvs[0].y},
                {QStringLiteral("u1"), uvs[1].x},
                {QStringLiteral("v1"), uvs[1].y},
                {QStringLiteral("u2"), uvs[2].x},
                {QStringLiteral("v2"), uvs[2].y},
                {QStringLiteral("island"), islandId},
                {QStringLiteral("color"), colorForIsland(islandId)}
            });
        };

        if (!sub.faces.empty()) {
            for (const auto& face : sub.faces) {
                if (!face.isValid())
                    continue;
                for (size_t i = 1; i + 1 < face.indices.size(); ++i) {
                    EditableTriangle tri;
                    tri.indices[0] = face.indices[0];
                    tri.indices[1] = face.indices[i];
                    tri.indices[2] = face.indices[i + 1];
                    emitTriangle(tri);
                }
                ++heFaceIdx;
            }
        } else {
            for (const auto& tri : sub.triangles) {
                emitTriangle(tri);
                ++heFaceIdx;
            }
        }
    }

    m_triangles = tris;
    m_islandCount = islands.islandCount;
    m_hasMesh = !tris.isEmpty();
    if (m_hasMesh) {
        if (!std::isfinite(uMin))
            m_uvBounds = QRectF(0, 0, 1, 1);
        else
            m_uvBounds = QRectF(uMin, vMin,
                                  std::max(1e-4f, uMax - uMin),
                                  std::max(1e-4f, vMax - vMin));
    } else {
        m_uvBounds = QRectF(0, 0, 1, 1);
    }

    const int previewSub = submeshFilter.isEmpty()
        ? 0
        : *std::min_element(submeshFilter.begin(), submeshFilter.end());
    m_textureBackgroundSource = resolveDiffuseTextureSource(entity, previewSub);
    return m_hasMesh;
}

void UVEditorController::rebuildMeshCache()
{
    m_triangles.clear();
    m_hasMesh = false;
    m_islandCount = 0;
    m_textureBackgroundSource.clear();
    m_uvBounds = QRectF(0, 0, 1, 1);
    m_statusText = tr("Select a mesh to view UVs.");

    auto* sel = SelectionSet::getSingleton();
    if (!sel) {
        ++m_meshRevision;
        emit meshDataChanged();
        return;
    }

    Ogre::Entity* entity = nullptr;
    QSet<int> submeshFilter;

    if (sel->hasSubEntities()) {
        for (Ogre::SubEntity* sub : sel->getSubEntitiesSelectionList()) {
            if (!sub)
                continue;
            Ogre::Entity* ent = sub->getParent();
            if (!ent)
                continue;
            if (!entity)
                entity = ent;
            if (ent != entity)
                continue;
            for (unsigned int si = 0; si < ent->getNumSubEntities(); ++si) {
                if (ent->getSubEntity(si) == sub) {
                    submeshFilter.insert(static_cast<int>(si));
                    break;
                }
            }
        }
    }

    if (!entity) {
        const auto entities = sel->getResolvedEntities();
        if (!entities.isEmpty())
            entity = entities.first();
    }

    if (!entity) {
        ++m_meshRevision;
        emit meshDataChanged();
        return;
    }

    m_statusText = submeshFilter.isEmpty()
        ? tr("UV layout — %1").arg(QString::fromStdString(entity->getName()))
        : tr("UV layout — %1 (sub-mesh selection)").arg(QString::fromStdString(entity->getName()));

    buildFromEntity(entity, submeshFilter, m_uvChannel);
    ++m_meshRevision;
    emit meshDataChanged();
}
