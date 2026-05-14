#include "TexturePaintController.h"

#include "EditModeController.h"
#include "EditableMesh.h"
#include "OgreWidget.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "SpaceCamera.h"
#include "UndoManager.h"
#include "VertexColorBaker.h"

#include <QApplication>
#include <QBuffer>
#include <QPainter>
#include <QPen>
#include <QTimer>
#include <QByteArray>
#include <QColorDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QUndoCommand>
#include <QVariantMap>
#include <QWidget>
#include <QtMath>

#include <OgreCamera.h>
#include <OgreEntity.h>
#include <OgreHardwarePixelBuffer.h>
#include <OgreImage.h>
#include <OgreManualObject.h>
#include <OgreMaterial.h>
#include <OgreMaterialManager.h>
#include <OgrePass.h>
#include <OgrePixelFormat.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSubEntity.h>
#include <OgreTechnique.h>
#include <OgreTextureManager.h>
#include <OgreTextureUnitState.h>

#include <algorithm>
#include <cstring>
#include <set>

TexturePaintController* TexturePaintController::s_instance = nullptr;

namespace {

/// Undo command for one texture-paint stroke. Snapshots the buffer
/// before/after and restores via memcpy into the Ogre texture.
class TexturePaintStrokeCommand : public QUndoCommand
{
public:
    TexturePaintStrokeCommand(TexturePaintController* controller,
                              std::vector<uint8_t> before,
                              std::vector<uint8_t> after,
                              int width,
                              int height,
                              QString textureName)
        : QUndoCommand(QStringLiteral("Texture paint"))
        , m_controller(controller)
        , m_before(std::move(before))
        , m_after(std::move(after))
        , m_width(width)
        , m_height(height)
        , m_textureName(std::move(textureName))
    {
    }

    void undo() override { apply(m_before); }
    void redo() override
    {
        if (m_skipFirstRedo) {
            m_skipFirstRedo = false;
            return;
        }
        apply(m_after);
    }

private:
    void apply(const std::vector<uint8_t>& pixels)
    {
        if (!m_controller) return;
        if (m_controller->currentTextureName() != m_textureName) return;
        const auto& buf = m_controller->buffer();
        if (buf.width() != m_width || buf.height() != m_height) return;
        m_controller->applyPixelSnapshot(pixels);
    }

    TexturePaintController* m_controller = nullptr;
    std::vector<uint8_t> m_before;
    std::vector<uint8_t> m_after;
    int m_width = 0;
    int m_height = 0;
    QString m_textureName;
    bool m_skipFirstRedo = true; // command is pushed *after* the stroke applied
};

} // namespace

TexturePaintController* TexturePaintController::instance()
{
    if (!s_instance)
        s_instance = new TexturePaintController();
    return s_instance;
}

TexturePaintController* TexturePaintController::qmlInstance(QQmlEngine*, QJSEngine*)
{
    auto* p = instance();
    QQmlEngine::setObjectOwnership(p, QQmlEngine::CppOwnership);
    return p;
}

void TexturePaintController::kill()
{
    delete s_instance;
    s_instance = nullptr;
}

TexturePaintController::TexturePaintController(QObject* parent)
    : QObject(parent)
{
    // Mirror the toolbar brush settings — texture paint and vertex paint
    // share one source of truth so the user isn't juggling two sets of
    // controls. EditModeController owns the canonical values; we just
    // forward its change signal so the QML "Texture Paint" panel can
    // re-render the live brush preview.
    if (auto* em = EditModeController::instance()) {
        connect(em, &EditModeController::vertexPaintChanged,
                this, &TexturePaintController::texturePaintChanged);
    }

    // Refresh the texture slot list whenever the selection changes —
    // the user expects "select a mesh → see its textures" without any
    // explicit refresh action.
    if (auto* sel = SelectionSet::getSingleton()) {
        connect(sel, &SelectionSet::selectionChanged,
                this, &TexturePaintController::refreshSlots);
    }
}

TexturePaintController::~TexturePaintController()
{
    // Drop the manual objects on the scene before Ogre destructors
    // race us on shutdown. closeSession is safe to call repeatedly.
    closeSession();
}

void TexturePaintController::setTexturePaintEnabled(bool enabled)
{
    if (m_paintEnabled == enabled) return;
    m_paintEnabled = enabled;
    if (enabled) {
        // Texture paint needs a private EditableMesh built from the
        // selected entity. NB: we deliberately don't call
        // EditModeController::enterEditMode() — that would flip the
        // workspace mode to EditMode, kick the user out of Material
        // Mode, and (via the visibility hooks) silently disable us.
        refreshSlots();
        // Make sure a session exists so the first click actually
        // paints — without this the user clicks, nothing happens,
        // they have to find the "Create / Attach Texture" button.
        if (!hasActiveSession())
            ensurePaintableTexture(1024);
    }
    if (!enabled && m_strokeActive)
        endStroke();
    SentryReporter::addBreadcrumb("ui.action",
        enabled ? "Texture paint: enabled" : "Texture paint: disabled");
    emit texturePaintChanged();
}

void TexturePaintController::setBrushTool(int tool)
{
    BrushTool t = static_cast<BrushTool>(tool);
    if (t == m_tool) return;
    m_tool = t;
    m_smudgeHavePrev = false;
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Texture paint: tool = %1").arg(tool));
    emit brushToolChanged();
}

void TexturePaintController::setActiveSlotIndex(int index)
{
    if (index < 0 || index >= m_slots.size()) return;
    if (m_activeSlot == index) return;
    // Preserve the user's current buffer resolution across slot
    // switches — without this, jumping slots always falls back to 1024
    // even after the user picked 2048.
    const int preservedRes = m_buffer.width() > 0 ? m_buffer.width() : 1024;
    m_activeSlot = index;
    // Switching slots resets the buffer to the new slot's texture.
    closeSession();
    ensurePaintableTexture(preservedRes);
    emit slotsChanged();
}

QColor TexturePaintController::texturePaintColor() const
{
    auto* em = EditModeController::instance();
    return em ? em->vertexPaintColor() : QColor(255, 0, 0);
}

double TexturePaintController::texturePaintRadius() const
{
    auto* em = EditModeController::instance();
    return em ? em->vertexPaintRadius() : 0.05;
}

double TexturePaintController::texturePaintStrength() const
{
    auto* em = EditModeController::instance();
    return em ? em->vertexPaintStrength() : 0.75;
}

double TexturePaintController::texturePaintFalloff() const
{
    auto* em = EditModeController::instance();
    return em ? em->vertexPaintFalloff() : 0.5;
}

Ogre::Entity* TexturePaintController::activeEntity() const
{
    // First selected entity in Material Mode = paint target. Edit Mode
    // is no longer involved — we keep our own EditableMesh so painting
    // works regardless of workspace mode.
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return nullptr;
    auto entities = sel->getResolvedEntities();
    return entities.isEmpty() ? nullptr : entities.first();
}

bool TexturePaintController::ensureEditableMesh(Ogre::Entity* entity)
{
    if (!entity) return false;
    if (m_paintMeshEntity == entity && m_paintMesh) return true;
    auto mesh = std::make_unique<EditableMesh>();
    if (!mesh->loadFromEntity(entity)) {
        m_paintMesh.reset();
        m_paintMeshEntity = nullptr;
        return false;
    }
    m_paintMesh = std::move(mesh);
    m_paintMeshEntity = entity;
    return true;
}

Ogre::TextureUnitState* TexturePaintController::findOrCreateActiveTextureUnit(Ogre::Entity* entity)
{
    if (!entity || entity->getNumSubEntities() == 0) return nullptr;

    // If we have a slot model, use the slot's recorded submesh +
    // texture name to find the matching TUS.
    if (m_activeSlot >= 0 && m_activeSlot < m_slots.size()) {
        auto m = m_slots.at(m_activeSlot).toMap();
        const int subIdx = m.value("submesh", -1).toInt();
        const std::string slotName = m.value("slot").toString().toStdString();
        if (subIdx >= 0 && subIdx < static_cast<int>(entity->getNumSubEntities())) {
            auto* subEnt = entity->getSubEntity(subIdx);
            if (subEnt) {
                Ogre::MaterialPtr mat = subEnt->getMaterial();
                if (mat && mat->getNumTechniques() > 0) {
                    auto* tech = mat->getTechnique(0);
                    if (tech && tech->getNumPasses() > 0) {
                        auto* pass = tech->getPass(0);
                        if (pass) {
                            for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
                                auto* tus = pass->getTextureUnitState(i);
                                if (tus->getName() == slotName)
                                    return tus;
                            }
                            // Slot named in model but not found on the pass
                            // (rare — model out of sync). Fall through to
                            // first TUS / create.
                        }
                    }
                }
            }
        }
    }

    // Fallback: first submesh, first material, first pass, prefer
    // canonical diffuse slot names; otherwise first TUS or create.
    auto* subEnt = entity->getSubEntity(0);
    if (!subEnt) return nullptr;
    Ogre::MaterialPtr mat = subEnt->getMaterial();
    if (!mat || mat->getNumTechniques() == 0) return nullptr;
    auto* tech = mat->getTechnique(0);
    if (!tech || tech->getNumPasses() == 0) return nullptr;
    auto* pass = tech->getPass(0);
    if (!pass) return nullptr;
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        const std::string& n = tus->getName();
        if (n == "albedo" || n == "diffuse_map")
            return tus;
    }
    if (pass->getNumTextureUnitStates() > 0)
        return pass->getTextureUnitState(0);
    return pass->createTextureUnitState();
}

bool TexturePaintController::ensurePaintableTexture(int resolution)
{
    auto* entity = activeEntity();
    if (!entity) {
        m_sessionEntity = nullptr;
        emit sessionChanged();
        return false;
    }

    if (!ensureEditableMesh(entity)) {
        // No mesh data → can't UV-hit-test → can't paint.
        emit sessionChanged();
        return false;
    }

    if (m_sessionEntity == entity && m_buffer.width() > 0 && !m_textureName.isEmpty())
        return true;

    // Reset any prior session.
    closeSession();
    m_sessionEntity = entity;

    auto* tu = findOrCreateActiveTextureUnit(entity);
    if (!tu) {
        emit sessionChanged();
        return false;
    }

    QString existingTex = QString::fromStdString(tu->getTextureName());
    bool loadedExisting = false;
    QString loadError;
    if (!existingTex.isEmpty()) {
        auto existing = Ogre::TextureManager::getSingleton().getByName(
            existingTex.toStdString(),
            Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
        if (existing) {
            try {
                // Make sure the texture is loaded (Ogre defers loading).
                if (!existing->isLoaded()) existing->load();
                Ogre::Image img;
                existing->convertToImage(img);
                const int w = static_cast<int>(img.getWidth());
                const int h = static_cast<int>(img.getHeight());
                if (w > 0 && h > 0) {
                    m_buffer.resize(w, h);
                    Ogre::PixelBox srcBox = img.getPixelBox();
                    Ogre::PixelBox dstBox(w, h, 1, Ogre::PF_BYTE_RGBA,
                                          m_buffer.data().data());
                    Ogre::PixelUtil::bulkPixelConversion(srcBox, dstBox);
                    m_buffer.clearDirty();
                    loadedExisting = true;
                }
            } catch (const Ogre::Exception& e) {
                loadError = QString::fromStdString(e.getDescription());
            } catch (...) {
                loadError = QStringLiteral("unknown exception");
            }
        } else {
            loadError = QStringLiteral("texture not found in TextureManager");
        }
    }

    if (!loadedExisting) {
        const int res = std::max(16, resolution);
        m_buffer.resize(res, res);
        m_buffer.clear(Ogre::ColourValue::White);
        m_buffer.clearDirty();
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Texture paint: starting from blank %1×%1 (tex='%2', err='%3')")
                .arg(res).arg(existingTex).arg(loadError));
    } else {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Texture paint: loaded existing %1×%2 from '%3'")
                .arg(m_buffer.width()).arg(m_buffer.height()).arg(existingTex));
    }

    static unsigned int s_unique = 0;
    QString hint = QStringLiteral("QMEPaint_%1_%2")
                       .arg(QString::fromStdString(entity->getName()))
                       .arg(++s_unique);
    if (!createOgreTextureFromBuffer(entity, hint)) {
        m_buffer = TexturePaintBuffer();
        m_textureName.clear();
        m_sessionEntity = nullptr;
        emit sessionChanged();
        return false;
    }

    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Texture paint session: %1×%2 on %3 (existing tex: %4)")
            .arg(m_buffer.width()).arg(m_buffer.height())
            .arg(QString::fromStdString(entity->getName()))
            .arg(loadedExisting ? "yes" : "no"));

    refreshPreviewUri();
    if (m_uvOverlayVisible) refreshUvOverlay();
    emit sessionChanged();
    return true;
}

bool TexturePaintController::createOgreTextureFromBuffer(Ogre::Entity* entity, const QString& nameHint)
{
    if (!entity || m_buffer.width() <= 0 || m_buffer.height() <= 0) return false;
    auto* tu = findOrCreateActiveTextureUnit(entity);
    if (!tu) return false;

    // Snapshot the original texture name on the chosen TUS *before* we
    // overwrite anything. We use this to find every other TUS bound to
    // the same source texture (e.g. `albedo` + `diffuse_map` aliasing
    // on imported PBR materials), so all of them get rebound.
    const std::string originalTexName = tu->getTextureName();

    const std::string texName = nameHint.toStdString();
    const std::string group = Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME;
    try {
        auto& tm = Ogre::TextureManager::getSingleton();
        if (auto existing = tm.getByName(texName, group))
            tm.remove(existing);
        m_ogreTexture = tm.createManual(
            texName, group, Ogre::TEX_TYPE_2D,
            m_buffer.width(), m_buffer.height(), 0,
            Ogre::PF_BYTE_RGBA,
            Ogre::TU_DYNAMIC_WRITE_ONLY_DISCARDABLE);
        if (!m_ogreTexture) return false;
        // Initial upload
        auto pixbuf = m_ogreTexture->getBuffer();
        if (!pixbuf) return false;
        Ogre::PixelBox pb(m_buffer.width(), m_buffer.height(), 1,
                          Ogre::PF_BYTE_RGBA, m_buffer.data().data());
        pixbuf->blitFromMemory(pb);
        m_textureName = QString::fromStdString(texName);

        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Texture paint: rebind base = '%1' → '%2'")
                .arg(QString::fromStdString(originalTexName))
                .arg(QString::fromStdString(texName)));
        // Rebind every TUS pointing at the user's original slot
        // texture. Track each rebind so closeSession() restores them.
        //
        // Imported PBR materials alias the diffuse texture under both
        // `diffuse_map` (TUS 0) and `albedo` (last TUS) — we rebind
        // both copies that share originalTexName.
        //
        // If originalTexName is empty (the slot had no texture bound
        // yet — e.g. a freshly-created session on a flat-color
        // material), we bind the *chosen slot only* so the user has
        // SOMETHING to paint against and see the result.
        m_boundSlots.clear();
        std::set<Ogre::Material*> touched;
        const int chosenSubmesh =
            (m_activeSlot >= 0 && m_activeSlot < m_slots.size())
                ? m_slots.at(m_activeSlot).toMap().value("submesh", -1).toInt()
                : 0;
        for (unsigned int se = 0; se < entity->getNumSubEntities(); ++se) {
            auto* sub = entity->getSubEntity(se);
            if (!sub) continue;
            Ogre::MaterialPtr mat = sub->getMaterial();
            if (!mat) continue;
            bool changed = false;
            const auto& techs = mat->getTechniques();
            for (size_t tIdx = 0; tIdx < techs.size(); ++tIdx) {
                auto* tech = techs[tIdx];
                for (unsigned short pi = 0; pi < tech->getNumPasses(); ++pi) {
                    auto* p = tech->getPass(pi);
                    for (unsigned short ti = 0; ti < p->getNumTextureUnitStates(); ++ti) {
                        auto* tusN = p->getTextureUnitState(ti);
                        const std::string currentTex = tusN->getTextureName();
                        bool shouldBind = false;
                        if (!originalTexName.empty()) {
                            shouldBind = (currentTex == originalTexName);
                        } else if (static_cast<int>(se) == chosenSubmesh) {
                            // Empty-source fallback: bind the exact TUS
                            // the user picked. We approximate "the
                            // user's TUS" as the matching slot/index on
                            // the chosen submesh.
                            shouldBind = (tusN == tu);
                        }
                        if (!shouldBind) continue;
                        BoundSlot s;
                        s.materialName = mat->getName();
                        s.techIdx = static_cast<unsigned short>(tIdx);
                        s.passIdx = pi;
                        s.tusIdx = ti;
                        s.originalTexture = currentTex;
                        m_boundSlots.push_back(s);
                        tusN->setTextureName(texName);
                        changed = true;
                    }
                }
            }
            if (changed) touched.insert(mat.get());
        }
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Texture paint: bound %1 TUSes").arg(m_boundSlots.size()));

        // Force RTSS / FFP lighting passes to drop their cached binding
        // and re-sample our texture. Without compile()+reload() the
        // renderer keeps using whatever sampler the shader was
        // generated with, so the new texture stays invisible.
        for (auto* mat : touched) {
            try {
                mat->compile();
                mat->reload();
            } catch (const Ogre::Exception&) {
                // Best-effort: a reload failure shouldn't kill the
                // paint session — the worst case is stale rendering
                // until the next material edit.
            }
        }
        return true;
    } catch (const Ogre::Exception&) {
        return false;
    } catch (...) {
        return false;
    }
}

void TexturePaintController::flushDirtyToOgre()
{
    if (!m_ogreTexture) return;
    const auto& dirty = m_buffer.dirtyRect();
    if (dirty.empty()) return;
    try {
        auto buf = m_ogreTexture->getBuffer();
        if (!buf) return;
        const int W = m_buffer.width();
        const int rectW = dirty.width();
        const int rectH = dirty.height();
        // Build a contiguous slice from the dirty rect.
        std::vector<uint8_t> slice(static_cast<size_t>(rectW) * static_cast<size_t>(rectH) * 4u);
        const auto& src = m_buffer.data();
        for (int row = 0; row < rectH; ++row) {
            const size_t srcOff = (static_cast<size_t>(dirty.y0 + row) * static_cast<size_t>(W)
                                   + static_cast<size_t>(dirty.x0)) * 4u;
            const size_t dstOff = static_cast<size_t>(row) * static_cast<size_t>(rectW) * 4u;
            std::memcpy(slice.data() + dstOff, src.data() + srcOff,
                        static_cast<size_t>(rectW) * 4u);
        }
        Ogre::PixelBox pb(rectW, rectH, 1, Ogre::PF_BYTE_RGBA, slice.data());
        Ogre::Box dst(dirty.x0, dirty.y0, dirty.x1, dirty.y1);
        buf->blitFromMemory(pb, dst);
    } catch (const Ogre::Exception& e) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Texture paint: blit failed — %1")
                .arg(QString::fromStdString(e.getDescription())));
    }
    m_buffer.clearDirty();
    // Debounce the 2D preview refresh — encoding a 1024×1024 PNG +
    // base64 on every stroke move is ~150ms of CPU work, which makes
    // dragging hitchy. Schedule one refresh per ~60ms; the buffer ↔
    // preview drift during that window is invisible to the user.
    if (!m_previewRefreshScheduled) {
        m_previewRefreshScheduled = true;
        QTimer::singleShot(60, this, [this]() {
            m_previewRefreshScheduled = false;
            refreshPreviewUri();
        });
    }
}

bool TexturePaintController::hitTestUV(const QPoint& screenPos, OgreWidget* widget, Ogre::Vector2& outUV) const
{
    if (!m_paintMesh || !m_paintMeshEntity || !widget) return false;
    auto* mesh = m_paintMesh.get();
    auto* entity = m_paintMeshEntity;

    auto* spaceCam = widget->getSpaceCamera();
    auto* camera = spaceCam ? spaceCam->getCamera() : nullptr;
    if (!camera) return false;

    int vw = 0, vh = 0;
    widget->pixelSizeForCameraPicking(vw, vh);
    if (vw <= 0 || vh <= 0) return false;

    const Ogre::Real nx = static_cast<Ogre::Real>(screenPos.x()) / vw;
    const Ogre::Real ny = static_cast<Ogre::Real>(screenPos.y()) / vh;
    const Ogre::Ray ray = camera->getCameraToViewportRay(nx, ny);

    Ogre::SceneNode* node = entity->getParentSceneNode();
    Ogre::Affine3 worldToLocal = node ? node->_getFullTransform().inverse() : Ogre::Affine3::IDENTITY;
    Ogre::Vector3 localOrigin = worldToLocal * ray.getOrigin();
    Ogre::Vector3 localDir = worldToLocal.linear() * ray.getDirection();
    localDir.normalise();

    // Walk every triangle and keep the closest hit. (We don't have
    // EditModeController's optimized bbox/octree, but for typical asset
    // meshes the linear walk is fine.)
    Ogre::Real bestT = std::numeric_limits<Ogre::Real>::infinity();
    Ogre::Vector2 bestUV(0, 0);
    bool found = false;
    for (const auto& sub : mesh->subMeshes()) {
        for (size_t ti = 0; ti < sub.triangles.size(); ++ti) {
            const auto& tri = sub.triangles[ti];
            const auto& v0 = sub.vertices[tri.indices[0]];
            const auto& v1 = sub.vertices[tri.indices[1]];
            const auto& v2 = sub.vertices[tri.indices[2]];
            // Möller–Trumbore for explicit barycentric coords.
            const Ogre::Vector3 e1 = v1.position - v0.position;
            const Ogre::Vector3 e2 = v2.position - v0.position;
            const Ogre::Vector3 pvec = localDir.crossProduct(e2);
            const Ogre::Real det = e1.dotProduct(pvec);
            if (std::abs(det) < 1e-8f) continue;
            const Ogre::Real invDet = 1.0f / det;
            const Ogre::Vector3 tvec = localOrigin - v0.position;
            const Ogre::Real u = tvec.dotProduct(pvec) * invDet;
            if (u < 0.0f || u > 1.0f) continue;
            const Ogre::Vector3 qvec = tvec.crossProduct(e1);
            const Ogre::Real v = localDir.dotProduct(qvec) * invDet;
            if (v < 0.0f || u + v > 1.0f) continue;
            const Ogre::Real tHit = e2.dotProduct(qvec) * invDet;
            if (tHit <= 0.0f || tHit >= bestT) continue;
            if (!v0.hasUV || !v1.hasUV || !v2.hasUV) continue;
            const Ogre::Real w = 1.0f - u - v;
            bestT = tHit;
            bestUV = v0.uv * w + v1.uv * u + v2.uv * v;
            found = true;
        }
    }
    if (!found) return false;
    outUV = bestUV;
    return true;
}

bool TexturePaintController::beginStroke(OgreWidget* widget, const QPoint& screenPos)
{
    if (!m_paintEnabled || m_strokeActive) return false;
    if (!hasActiveSession()) {
        if (!ensurePaintableTexture(m_buffer.width() > 0 ? m_buffer.width() : 1024))
            return false;
    }
    m_strokeActive = true;
    m_strokeJustBegan = true;
    m_smudgeHavePrev = false;
    m_strokePreSnapshot = snapshotPixels();
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Texture paint stroke begin (tool=%1 radius=%2 strength=%3 color=%4)")
            .arg(static_cast<int>(m_tool))
            .arg(texturePaintRadius(), 0, 'f', 3)
            .arg(texturePaintStrength(), 0, 'f', 3)
            .arg(texturePaintColor().name(QColor::HexRgb)));
    updateStroke(widget, screenPos);
    return true;
}

void TexturePaintController::updateStroke(OgreWidget* widget, const QPoint& screenPos)
{
    if (!m_strokeActive || !m_paintEnabled) return;
    Ogre::Vector2 uv;
    if (!hitTestUV(screenPos, widget, uv)) {
        clearHoveredUV();
        return;
    }
    emit hoveredUVChanged(uv.x, uv.y);
    // Keep the brush-ring overlay tracking the cursor during a stroke.
    Ogre::Vector3 localPos, localNormal;
    if (findMeshPointForUV(uv, localPos, localNormal))
        drawHoverRingAt(localPos, localNormal);
    if (applyBrushAtUV(uv))
        flushDirtyToOgre();
}

bool TexturePaintController::applyBrushAtUV(const Ogre::Vector2& uv)
{
    if (m_buffer.width() <= 0) return false;
    // The shared brush radius is in local mesh units. Map it to UV
    // space by dividing by the mesh's bounding-box size — that's a
    // reasonable first approximation when UVs are unwrapped onto a
    // [0..1] square. Clamp to [0.005..1.0] so absurd sizes don't
    // produce zero-pixel or whole-texture stamps.
    float radius = static_cast<float>(texturePaintRadius());
    if (m_paintMesh) {
        const auto bbox = m_paintMesh->calculateBounds();
        if (bbox.isFinite()) {
            const float meshExtent = bbox.getSize().length() * 0.5f;
            if (meshExtent > 0.0f) {
                radius = static_cast<float>(texturePaintRadius()) / meshExtent;
            }
        }
    }
    radius = std::clamp(radius, 0.005f, 1.0f);
    const float strength = static_cast<float>(texturePaintStrength());
    const float falloff = static_cast<float>(texturePaintFalloff());

    switch (m_tool) {
    case ToolPaint: {
        const QColor c = texturePaintColor();
        const Ogre::ColourValue paint(c.redF(), c.greenF(), c.blueF(), c.alphaF());
        return m_buffer.paintBrush(uv, radius, paint, strength, falloff) > 0;
    }
    case ToolErase: {
        // Erase = paint transparent. Strength controls how much alpha
        // the stamp removes.
        const Ogre::ColourValue clear(0.0f, 0.0f, 0.0f, 0.0f);
        return m_buffer.paintBrush(uv, radius, clear, strength, falloff) > 0;
    }
    case ToolFill: {
        // Fill is a single-stamp operation — apply once per stroke
        // start, not on every move. Most paint apps work this way.
        // Suppress repeats by only flooding when the stroke just began.
        if (!m_strokeJustBegan) return false;
        m_strokeJustBegan = false;
        return floodFillAtUV(uv);
    }
    case ToolColorPicker: {
        if (!m_strokeJustBegan) return false;
        m_strokeJustBegan = false;
        pickColorAtUV(uv);
        return false;
    }
    case ToolSmudge: {
        // Sample previous stamp's pixels and bias toward them.
        if (!m_smudgeHavePrev) {
            m_smudgePrev = uv;
            m_smudgeHavePrev = true;
            return false;
        }
        // Walk pixels in the brush footprint; for each, sample at
        // (uv + (prev - uv) * smudgeAmount) and lerp toward it.
        const int W = m_buffer.width();
        const int H = m_buffer.height();
        const float radiusXf = radius * W;
        const float radiusYf = radius * H;
        const float cxF = uv.x * W;
        const float cyF = uv.y * H;
        const int x0 = std::max(0, static_cast<int>(std::floor(cxF - radiusXf)));
        const int x1 = std::min(W, static_cast<int>(std::ceil (cxF + radiusXf)));
        const int y0 = std::max(0, static_cast<int>(std::floor(cyF - radiusYf)));
        const int y1 = std::min(H, static_cast<int>(std::ceil (cyF + radiusYf)));
        if (x0 >= x1 || y0 >= y1) return false;
        const float dxUV = m_smudgePrev.x - uv.x;
        const float dyUV = m_smudgePrev.y - uv.y;
        const float p = 1.0f + falloff * 3.0f;
        const float invRx = 1.0f / std::max(radiusXf, 1e-6f);
        const float invRy = 1.0f / std::max(radiusYf, 1e-6f);
        bool changed = false;
        int touchedX0 = x1, touchedX1 = x0, touchedY0 = y1, touchedY1 = y0;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const float dx = (x + 0.5f - cxF) * invRx;
                const float dy = (y + 0.5f - cyF) * invRy;
                const float r2 = dx * dx + dy * dy;
                if (r2 >= 1.0f) continue;
                const float w = std::pow(1.0f - r2, p);
                const float blend = strength * w;
                if (blend <= 0.0f) continue;
                const Ogre::Vector2 sampleUV(
                    (x + 0.5f) / W + dxUV,
                    (y + 0.5f) / H + dyUV);
                int sx = 0, sy = 0;
                m_buffer.uvToPixel(sampleUV, sx, sy);
                const auto src = m_buffer.pixel(sx, sy);
                const auto dst = m_buffer.pixel(x, y);
                if (src.a <= 0.0f) continue;
                const Ogre::ColourValue blended(
                    dst.r + (src.r - dst.r) * blend,
                    dst.g + (src.g - dst.g) * blend,
                    dst.b + (src.b - dst.b) * blend,
                    dst.a + (src.a - dst.a) * blend);
                m_buffer.setPixel(x, y, blended);
                changed = true;
                touchedX0 = std::min(touchedX0, x);
                touchedY0 = std::min(touchedY0, y);
                touchedX1 = std::max(touchedX1, x + 1);
                touchedY1 = std::max(touchedY1, y + 1);
            }
        }
        m_smudgePrev = uv;
        return changed;
    }
    }
    return false;
}

bool TexturePaintController::floodFillAtUV(const Ogre::Vector2& uv)
{
    int sx = 0, sy = 0;
    m_buffer.uvToPixel(uv, sx, sy);
    const QColor c = texturePaintColor();
    const Ogre::ColourValue fill(c.redF(), c.greenF(), c.blueF(), c.alphaF());
    return m_buffer.floodFill(sx, sy, fill) > 0;
}

void TexturePaintController::pickColorAtUV(const Ogre::Vector2& uv)
{
    int x = 0, y = 0;
    m_buffer.uvToPixel(uv, x, y);
    const auto c = m_buffer.pixel(x, y);
    QColor qc;
    qc.setRgbF(c.r, c.g, c.b, 1.0f);
    if (auto* em = EditModeController::instance())
        em->setVertexPaintColor(qc);
}

void TexturePaintController::endStroke()
{
    if (!m_strokeActive) return;
    m_strokeActive = false;
    // If nothing changed, drop the snapshot.
    auto after = snapshotPixels();
    if (after == m_strokePreSnapshot) {
        m_strokePreSnapshot.clear();
        SentryReporter::addBreadcrumb("ui.action", "Texture paint stroke end (no changes)");
        return;
    }
    auto* cmd = new TexturePaintStrokeCommand(
        this,
        std::move(m_strokePreSnapshot),
        std::move(after),
        m_buffer.width(), m_buffer.height(),
        m_textureName);
    UndoManager::getSingleton()->push(cmd);
    SentryReporter::addBreadcrumb("ui.action", "Texture paint stroke end (committed)");
}

std::vector<uint8_t> TexturePaintController::snapshotPixels() const
{
    return m_buffer.data();
}

bool TexturePaintController::savePaintBuffer(const QString& path) const
{
    if (path.isEmpty()) return false;
    if (!hasActiveSession()) return false;
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());
    return m_buffer.save(path.toStdString());
}

bool TexturePaintController::loadPaintBuffer(const QString& path)
{
    if (path.isEmpty()) return false;
    if (!m_buffer.load(path.toStdString())) return false;
    auto* entity = activeEntity();
    if (entity) {
        QFileInfo fi(path);
        QString hint = QStringLiteral("QMEPaintLoad_%1").arg(fi.completeBaseName());
        m_sessionEntity = entity;
        // Re-create the Ogre texture at the new resolution.
        m_ogreTexture.reset();
        if (!createOgreTextureFromBuffer(entity, hint)) return false;
    }
    emit sessionChanged();
    return true;
}

QString TexturePaintController::savePaintBufferInteractive()
{
    if (!hasActiveSession()) {
        SentryReporter::addBreadcrumb("ui.action",
            "Texture paint: save dialog skipped (no session)");
        return QString();
    }
    QApplication::processEvents();
    QWidget* parent = QApplication::activeWindow();
    const QString path = QFileDialog::getSaveFileName(
        parent,
        QStringLiteral("Save painted texture"),
        QDir::currentPath() + "/paint.png",
        QStringLiteral("PNG image (*.png);;JPEG image (*.jpg *.jpeg);;TGA image (*.tga);;BMP image (*.bmp)"),
        nullptr,
        QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons);
    if (path.isEmpty()) return QString();
    if (!savePaintBuffer(path)) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Texture paint: save failed (%1)").arg(path));
        return QString();
    }
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Texture paint: saved to %1").arg(path));
    return path;
}

void TexturePaintController::pickBrushColorInteractive()
{
    auto* em = EditModeController::instance();
    if (!em) return;
    QApplication::processEvents();
    QWidget* parent = QApplication::activeWindow();
    const QColor picked = QColorDialog::getColor(
        em->vertexPaintColor(), parent, QStringLiteral("Brush color"));
    if (picked.isValid())
        em->setVertexPaintColor(picked);
}

void TexturePaintController::setBrushRadius(double r)
{
    if (auto* em = EditModeController::instance()) em->setVertexPaintRadius(r);
}

void TexturePaintController::setBrushStrength(double s)
{
    if (auto* em = EditModeController::instance()) em->setVertexPaintStrength(s);
}

void TexturePaintController::setBrushFalloff(double f)
{
    if (auto* em = EditModeController::instance()) em->setVertexPaintFalloff(f);
}

void TexturePaintController::setBrushColor(const QColor& c)
{
    if (auto* em = EditModeController::instance()) em->setVertexPaintColor(c);
}

QString TexturePaintController::loadPaintBufferInteractive()
{
    QApplication::processEvents();
    QWidget* parent = QApplication::activeWindow();
    const QString path = QFileDialog::getOpenFileName(
        parent,
        QStringLiteral("Load texture into paint buffer"),
        QDir::currentPath(),
        QStringLiteral("Image files (*.png *.jpg *.jpeg *.tga *.bmp);;All files (*)"),
        nullptr,
        QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons);
    if (path.isEmpty()) return QString();
    if (!loadPaintBuffer(path)) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Texture paint: load failed (%1)").arg(path));
        return QString();
    }
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Texture paint: loaded %1").arg(path));
    return path;
}

int TexturePaintController::bakeVertexColorsToTexture(int resolution,
                                                      int dilation,
                                                      const QString& savePath)
{
    auto* entity = activeEntity();
    if (!entity) return -1;
    if (!ensureEditableMesh(entity)) return -1;
    const int res = resolution > 0 ? resolution
                                   : (m_buffer.width() > 0 ? m_buffer.width() : 1024);
    VertexColorBaker::Options opts;
    opts.resolution = res;
    opts.dilationPixels = std::max(0, dilation);
    const int painted = VertexColorBaker::bake(*m_paintMesh, m_buffer, opts);

    m_sessionEntity = entity;
    m_ogreTexture.reset();
    static unsigned int s_bakeUnique = 0;
    const QString hint = QStringLiteral("QMEBake_%1_%2")
                       .arg(QString::fromStdString(entity->getName()))
                       .arg(++s_bakeUnique);
    createOgreTextureFromBuffer(entity, hint);

    if (!savePath.isEmpty())
        m_buffer.save(savePath.toStdString());

    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Vertex→Texture bake: %1×%1 (%2 pixels, dilation=%3)")
            .arg(res).arg(painted).arg(opts.dilationPixels));

    refreshPreviewUri();
    emit sessionChanged();
    return painted;
}

void TexturePaintController::applyPixelSnapshot(const std::vector<uint8_t>& pixels)
{
    if (m_buffer.width() <= 0 || m_buffer.height() <= 0) return;
    if (pixels.size() != m_buffer.data().size()) return;
    std::memcpy(m_buffer.data().data(), pixels.data(), pixels.size());
    m_buffer.markDirty(0, 0, m_buffer.width(), m_buffer.height());
    flushDirtyToOgre();
}

void TexturePaintController::closeSession()
{
    if (m_strokeActive) endStroke();

    // Restore every TUS we rebound. Must do this BEFORE removing the
    // paint texture from TextureManager, otherwise the next render
    // tries to sample a freed handle. Look the material up by name
    // each time so a destroyed/reloaded material doesn't dangle.
    std::set<std::string> toReload;
    for (const auto& s : m_boundSlots) {
        try {
            if (s.materialName.empty()) continue;
            auto matPtr = Ogre::MaterialManager::getSingleton().getByName(
                s.materialName);
            if (!matPtr) continue;
            const auto& techs = matPtr->getTechniques();
            if (s.techIdx >= techs.size()) continue;
            auto* tech = techs[s.techIdx];
            if (s.passIdx >= tech->getNumPasses()) continue;
            auto* p = tech->getPass(s.passIdx);
            if (s.tusIdx >= p->getNumTextureUnitStates()) continue;
            auto* tusN = p->getTextureUnitState(s.tusIdx);
            tusN->setTextureName(s.originalTexture);
            toReload.insert(s.materialName);
        } catch (...) {}
    }
    m_boundSlots.clear();
    for (const auto& mname : toReload) {
        try {
            auto matPtr = Ogre::MaterialManager::getSingleton().getByName(mname);
            if (matPtr) { matPtr->compile(); matPtr->reload(); }
        } catch (...) {}
    }

    if (m_ogreTexture) {
        try {
            Ogre::TextureManager::getSingleton().remove(m_ogreTexture);
        } catch (...) {}
        m_ogreTexture.reset();
    }
    if (m_ringObj && m_paintMeshEntity) {
        try {
            auto* mgr = m_paintMeshEntity->_getManager();
            if (mgr) {
                if (m_ringNode) {
                    m_ringNode->detachAllObjects();
                    mgr->getRootSceneNode()->removeChild(m_ringNode);
                    mgr->destroySceneNode(m_ringNode);
                    m_ringNode = nullptr;
                }
                mgr->destroyManualObject(m_ringObj);
                m_ringObj = nullptr;
            }
        } catch (...) {}
    }
    m_paintMesh.reset();
    m_paintMeshEntity = nullptr;
    m_buffer = TexturePaintBuffer();
    m_textureName.clear();
    m_sessionEntity = nullptr;
    m_strokePreSnapshot.clear();
    if (!m_uvOverlayUri.isEmpty()) {
        m_uvOverlayUri.clear();
        emit uvOverlayChanged();
    }
    m_previewUri.clear();
    emit previewChanged();
    emit sessionChanged();
}

// ---------------------------------------------------------------------------
// UV-based stroke API (driven by the texture preview panel)
// ---------------------------------------------------------------------------

bool TexturePaintController::beginStrokeUV(double u, double v)
{
    if (!m_paintEnabled || m_strokeActive) return false;
    if (!hasActiveSession())
        if (!ensurePaintableTexture(1024)) return false;
    m_strokeActive = true;
    m_strokeJustBegan = true;
    m_smudgeHavePrev = false;
    m_strokePreSnapshot = snapshotPixels();
    emit hoveredUVChanged(u, v);
    updateStrokeUV(u, v);
    return true;
}

void TexturePaintController::updateStrokeUV(double u, double v)
{
    if (!m_strokeActive || !m_paintEnabled) return;
    const Ogre::Vector2 uv(static_cast<float>(u), static_cast<float>(v));
    emit hoveredUVChanged(u, v);
    // Update brush-ring overlay on the mesh so the user sees their
    // painting location even when driving the brush from the 2D panel.
    Ogre::Vector3 localPos, localNormal;
    if (findMeshPointForUV(uv, localPos, localNormal))
        drawHoverRingAt(localPos, localNormal);
    if (applyBrushAtUV(uv))
        flushDirtyToOgre();
}

void TexturePaintController::endStrokeUV()
{
    endStroke();
}

void TexturePaintController::setHoveredUV(double u, double v)
{
    emit hoveredUVChanged(u, v);
    // Reverse-lookup: find the 3D point on the mesh that maps to this
    // UV, then draw the brush ring there. Lets the user "scrub" the
    // texture panel and see the corresponding location on the model.
    if (!m_paintEnabled || !m_paintMesh || !m_paintMeshEntity) return;
    Ogre::Vector3 localPos, localNormal;
    if (findMeshPointForUV(Ogre::Vector2(u, v), localPos, localNormal))
        drawHoverRingAt(localPos, localNormal);
}

void TexturePaintController::clearHoveredUV()
{
    emit hoveredUVChanged(-1.0, -1.0);
}

// ---------------------------------------------------------------------------
// Texture slot enumeration
// ---------------------------------------------------------------------------

void TexturePaintController::refreshSlots()
{
    // If paint is enabled and the user just changed selection, try to
    // re-establish the session against the new entity so they don't
    // have to click "Create / Attach Texture" again. Skip if the
    // session is already valid for the current entity.
    if (m_paintEnabled) {
        auto* e = activeEntity();
        if (e && e != m_sessionEntity && !hasActiveSession())
            ensurePaintableTexture(1024);
    }

    QVariantList newSlots;
    auto* entity = activeEntity();
    if (entity) {
        for (unsigned int si = 0; si < entity->getNumSubEntities(); ++si) {
            auto* subEnt = entity->getSubEntity(si);
            if (!subEnt) continue;
            Ogre::MaterialPtr mat = subEnt->getMaterial();
            if (!mat || mat->getNumTechniques() == 0) continue;
            auto* tech = mat->getTechnique(0);
            if (!tech || tech->getNumPasses() == 0) continue;
            auto* pass = tech->getPass(0);
            if (!pass) continue;
            for (unsigned short ti = 0; ti < pass->getNumTextureUnitStates(); ++ti) {
                auto* tus = pass->getTextureUnitState(ti);
                const std::string& n = tus->getName();
                const std::string tex = tus->getTextureName();
                // Skip entirely-empty TUSes (no name AND no texture) —
                // they're usually placeholders.
                if (n.empty() && tex.empty()) continue;
                QVariantMap m;
                const QString labelN = QString::fromStdString(n.empty() ? "TUS " + std::to_string(ti) : n);
                m["label"] = QStringLiteral("sub %1 — %2").arg(si).arg(labelN);
                m["submesh"] = static_cast<int>(si);
                m["slot"] = QString::fromStdString(n);
                m["textureName"] = QString::fromStdString(tex);
                newSlots.append(m);
            }
        }
    }
    if (newSlots == m_slots) return;
    m_slots = newSlots;
    if (m_activeSlot >= m_slots.size())
        m_activeSlot = m_slots.isEmpty() ? -1 : 0;
    emit slotsChanged();
}

// ---------------------------------------------------------------------------
// Preview URI
// ---------------------------------------------------------------------------

void TexturePaintController::setUvOverlayVisible(bool on)
{
    if (m_uvOverlayVisible == on) return;
    m_uvOverlayVisible = on;
    if (on && m_uvOverlayUri.isEmpty()) refreshUvOverlay();
    emit uvOverlayChanged();
}

void TexturePaintController::refreshUvOverlay()
{
    if (!m_paintMesh || m_buffer.width() <= 0 || m_buffer.height() <= 0) {
        if (!m_uvOverlayUri.isEmpty()) {
            m_uvOverlayUri.clear();
            emit uvOverlayChanged();
        }
        return;
    }
    const int W = m_buffer.width();
    const int H = m_buffer.height();
    QImage img(W, H, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, false);
    QPen pen(QColor(255, 255, 255, 200));
    // Keep the wireframe a single pixel wide at any resolution.
    pen.setWidth(1);
    pen.setCosmetic(true);
    p.setPen(pen);

    auto toPx = [&](const Ogre::Vector2& uv) {
        return QPointF(uv.x * W, uv.y * H);
    };
    for (const auto& sub : m_paintMesh->subMeshes()) {
        for (const auto& tri : sub.triangles) {
            const auto& v0 = sub.vertices[tri.indices[0]];
            const auto& v1 = sub.vertices[tri.indices[1]];
            const auto& v2 = sub.vertices[tri.indices[2]];
            if (!v0.hasUV || !v1.hasUV || !v2.hasUV) continue;
            const QPointF p0 = toPx(v0.uv);
            const QPointF p1 = toPx(v1.uv);
            const QPointF p2 = toPx(v2.uv);
            p.drawLine(p0, p1);
            p.drawLine(p1, p2);
            p.drawLine(p2, p0);
        }
    }
    p.end();

    QByteArray bytes;
    QBuffer qbuf(&bytes);
    qbuf.open(QIODevice::WriteOnly);
    img.save(&qbuf, "PNG");
    const QString next = QStringLiteral("data:image/png;base64,")
                       + QString::fromLatin1(bytes.toBase64());
    if (next != m_uvOverlayUri) {
        m_uvOverlayUri = next;
        emit uvOverlayChanged();
    }
}

void TexturePaintController::refreshPreviewUri()
{
    if (m_buffer.width() <= 0 || m_buffer.height() <= 0) {
        if (!m_previewUri.isEmpty()) {
            m_previewUri.clear();
            emit previewChanged();
        }
        return;
    }
    QImage img(const_cast<uchar*>(m_buffer.data().data()),
               m_buffer.width(), m_buffer.height(),
               m_buffer.width() * 4, QImage::Format_RGBA8888);
    QByteArray bytes;
    QBuffer qbuf(&bytes);
    qbuf.open(QIODevice::WriteOnly);
    img.save(&qbuf, "PNG");
    const QString next = QStringLiteral("data:image/png;base64,")
                       + QString::fromLatin1(bytes.toBase64());
    if (next != m_previewUri) {
        m_previewUri = next;
        emit previewChanged();
    }
}

// ---------------------------------------------------------------------------
// 3D-mesh hover ring overlay
// ---------------------------------------------------------------------------

void TexturePaintController::updateMeshHover(OgreWidget* widget, const QPoint& screenPos)
{
    if (!m_paintEnabled) { clearMeshHover(); return; }
    Ogre::Vector2 uv;
    if (!hitTestUV(screenPos, widget, uv)) {
        clearMeshHover();
        return;
    }
    emit hoveredUVChanged(uv.x, uv.y);

    // Compute the 3D hit point in local space and a surface normal.
    // We piggyback on the same hit-test math: re-fire a ray and find
    // the closest triangle (same logic as hitTestUV; we keep it inline
    // here to also recover the world-space hit position and normal).
    auto* mesh = m_paintMesh.get();
    auto* entity = m_paintMeshEntity;
    if (!mesh || !entity) return;
    auto* spaceCam = widget->getSpaceCamera();
    auto* camera = spaceCam ? spaceCam->getCamera() : nullptr;
    if (!camera) return;
    int vw = 0, vh = 0;
    widget->pixelSizeForCameraPicking(vw, vh);
    if (vw <= 0 || vh <= 0) return;
    const Ogre::Real nx = static_cast<Ogre::Real>(screenPos.x()) / vw;
    const Ogre::Real ny = static_cast<Ogre::Real>(screenPos.y()) / vh;
    const Ogre::Ray ray = camera->getCameraToViewportRay(nx, ny);
    Ogre::SceneNode* node = entity->getParentSceneNode();
    Ogre::Affine3 worldToLocal = node ? node->_getFullTransform().inverse() : Ogre::Affine3::IDENTITY;
    Ogre::Vector3 localOrigin = worldToLocal * ray.getOrigin();
    Ogre::Vector3 localDir = worldToLocal.linear() * ray.getDirection();
    localDir.normalise();

    Ogre::Vector3 hitLocal(0,0,0);
    Ogre::Vector3 hitNormal(0,1,0);
    Ogre::Real bestT = std::numeric_limits<Ogre::Real>::infinity();
    for (const auto& sub : mesh->subMeshes()) {
        for (const auto& tri : sub.triangles) {
            const auto& v0 = sub.vertices[tri.indices[0]];
            const auto& v1 = sub.vertices[tri.indices[1]];
            const auto& v2 = sub.vertices[tri.indices[2]];
            const Ogre::Vector3 e1 = v1.position - v0.position;
            const Ogre::Vector3 e2 = v2.position - v0.position;
            const Ogre::Vector3 pvec = localDir.crossProduct(e2);
            const Ogre::Real det = e1.dotProduct(pvec);
            if (std::abs(det) < 1e-8f) continue;
            const Ogre::Real invDet = 1.0f / det;
            const Ogre::Vector3 tvec = localOrigin - v0.position;
            const Ogre::Real u = tvec.dotProduct(pvec) * invDet;
            if (u < 0.0f || u > 1.0f) continue;
            const Ogre::Vector3 qvec = tvec.crossProduct(e1);
            const Ogre::Real v = localDir.dotProduct(qvec) * invDet;
            if (v < 0.0f || u + v > 1.0f) continue;
            const Ogre::Real tHit = e2.dotProduct(qvec) * invDet;
            if (tHit <= 0.0f || tHit >= bestT) continue;
            bestT = tHit;
            hitLocal = localOrigin + localDir * tHit;
            hitNormal = e1.crossProduct(e2);
            if (!hitNormal.isZeroLength()) hitNormal.normalise();
        }
    }
    if (bestT == std::numeric_limits<Ogre::Real>::infinity()) {
        clearMeshHover();
        return;
    }
    drawHoverRingAt(hitLocal, hitNormal);
}

void TexturePaintController::clearMeshHover()
{
    if (m_ringObj) m_ringObj->clear();
    emit hoveredUVChanged(-1.0, -1.0);
}

bool TexturePaintController::findMeshPointForUV(const Ogre::Vector2& uv,
                                                Ogre::Vector3& outLocal,
                                                Ogre::Vector3& outNormal) const
{
    if (!m_paintMesh) return false;
    // Walk every triangle, find the one whose UV-space contains `uv`
    // (barycentric test on UV triangle), then interpolate the 3D
    // position with those same barycentrics.
    for (const auto& sub : m_paintMesh->subMeshes()) {
        for (const auto& tri : sub.triangles) {
            const auto& v0 = sub.vertices[tri.indices[0]];
            const auto& v1 = sub.vertices[tri.indices[1]];
            const auto& v2 = sub.vertices[tri.indices[2]];
            if (!v0.hasUV || !v1.hasUV || !v2.hasUV) continue;
            // Barycentric coords for `uv` in the UV triangle (v0.uv, v1.uv, v2.uv).
            const Ogre::Vector2 e1 = v1.uv - v0.uv;
            const Ogre::Vector2 e2 = v2.uv - v0.uv;
            const Ogre::Vector2 dp = uv  - v0.uv;
            const float denom = e1.x * e2.y - e2.x * e1.y;
            if (std::abs(denom) < 1e-10f) continue;
            const float u = (dp.x * e2.y - e2.x * dp.y) / denom;
            const float v = (e1.x * dp.y - dp.x * e1.y) / denom;
            const float w = 1.0f - u - v;
            const float eps = 1e-4f;
            if (u < -eps || v < -eps || w < -eps) continue;
            outLocal = v0.position * w + v1.position * u + v2.position * v;
            Ogre::Vector3 n = (v1.position - v0.position)
                .crossProduct(v2.position - v0.position);
            if (!n.isZeroLength()) n.normalise();
            else n = Ogre::Vector3::UNIT_Y;
            outNormal = n;
            return true;
        }
    }
    return false;
}

void TexturePaintController::drawHoverRingAt(const Ogre::Vector3& localPos,
                                              const Ogre::Vector3& localNormal)
{
    auto* entity = m_paintMeshEntity;
    if (!entity) return;
    auto* sceneMgr = entity->_getManager();
    if (!sceneMgr) return;

    // Ensure the hover-line material exists. It's normally created
    // when entering Edit Mode (EditModeController::createOverlayMaterials);
    // we may need to draw the ring before the user has ever entered
    // Edit Mode, so set it up here too.
    static const char* kMatName = "TexturePaint/HoverRing";
    auto& matMgr = Ogre::MaterialManager::getSingleton();
    if (!matMgr.getByName(kMatName)) {
        auto mat = matMgr.create(kMatName,
            Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
        auto* pass = mat->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(false);
        pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
        pass->setDepthCheckEnabled(false);
        pass->setDepthWriteEnabled(false);
        pass->setLineWidth(2.0f);
    }
    if (!m_ringNode) m_ringNode = sceneMgr->getRootSceneNode()->createChildSceneNode();
    if (!m_ringObj) {
        m_ringObj = sceneMgr->createManualObject("TexturePaint_HoverRing");
        m_ringObj->setDynamic(true);
        m_ringObj->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY);
        m_ringNode->attachObject(m_ringObj);
    }
    auto* node = entity->getParentSceneNode();
    if (node) {
        m_ringNode->setPosition(node->_getDerivedPosition());
        m_ringNode->setOrientation(node->_getDerivedOrientation());
        m_ringNode->setScale(node->_getDerivedScale());
    }
    m_ringObj->clear();

    Ogre::Vector3 normal = localNormal;
    if (normal.squaredLength() < 1e-6f) normal = Ogre::Vector3::UNIT_Y;
    Ogre::Vector3 tangent = normal.perpendicular();
    tangent.normalise();
    Ogre::Vector3 bitangent = normal.crossProduct(tangent);
    bitangent.normalise();

    const QColor c = texturePaintColor();
    const Ogre::ColourValue ringCol(c.redF(), c.greenF(), c.blueF(), 0.95f);
    constexpr int kSegments = 64;
    // The brush radius is in local mesh units (shared with vertex paint).
    // 0.8 narrows the ring slightly so it visually matches the painted
    // footprint (a softly-falloff brush doesn't quite touch the ring edge).
    const float radius = static_cast<float>(texturePaintRadius()) * 0.8f;
    const Ogre::Vector3 center = localPos + normal * 0.001f;
    m_ringObj->begin(kMatName, Ogre::RenderOperation::OT_LINE_STRIP);
    for (int i = 0; i <= kSegments; ++i) {
        const float t = static_cast<float>(i) / kSegments;
        const float a = Ogre::Math::TWO_PI * t;
        const Ogre::Vector3 p = center
            + (tangent * Ogre::Math::Cos(a) + bitangent * Ogre::Math::Sin(a)) * radius;
        m_ringObj->position(p);
        m_ringObj->colour(ringCol);
    }
    m_ringObj->end();
}
