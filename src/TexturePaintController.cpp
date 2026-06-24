#include "TexturePaintController.h"

#include "EditModeController.h"
#include "EditableMesh.h"
#include "Manager.h"
#include "OgreWidget.h"
#include "PaintBufferImageProvider.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "SpaceCamera.h"
#include "UndoManager.h"
#include "VertexColorBaker.h"
#include "EmbeddedTextureCache.h"

#include <QApplication>
#include <QBuffer>
#include <QPainter>
#include <QPen>
#include <QLibraryInfo>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QTimer>
#include <QByteArray>
#include <QColorDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QUndoCommand>
#include <QUrl>
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

/// Undo command for a one-shot selection-mask action (fill FG / fill BG
/// / delete). Same pre/post-snapshot model as TexturePaintStrokeCommand
/// — actions like "Delete selected" can affect thousands of pixels but
/// happen atomically, so storing a full pixel snapshot is the simplest
/// correct approach (matches Photoshop's "History snapshot").
class TexturePaintMaskActionCommand : public QUndoCommand
{
public:
    TexturePaintMaskActionCommand(TexturePaintController* controller,
                                  std::vector<uint8_t> before,
                                  std::vector<uint8_t> after,
                                  int width,
                                  int height,
                                  QString textureName,
                                  QString label)
        : QUndoCommand(label)
        , m_controller(controller)
        , m_before(std::move(before))
        , m_after(std::move(after))
        , m_width(width)
        , m_height(height)
        , m_textureName(std::move(textureName))
    {}

    void undo() override { apply(m_before); }
    void redo() override
    {
        if (m_skipFirstRedo) { m_skipFirstRedo = false; return; }
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
    bool m_skipFirstRedo = true;
};

Ogre::TexturePtr findTextureAcrossGroups(const std::string& name)
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

bool copyQImageToPaintBuffer(TexturePaintBuffer& buffer, const QImage& source)
{
    QImage qimg = source;
    if (qimg.isNull())
        return false;
    if (qimg.format() != QImage::Format_RGBA8888)
        qimg = qimg.convertToFormat(QImage::Format_RGBA8888);
    const int w = qimg.width();
    const int h = qimg.height();
    if (w <= 0 || h <= 0)
        return false;
    buffer.resize(w, h);
    for (int y = 0; y < h; ++y) {
        std::memcpy(buffer.data().data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u,
                    qimg.constScanLine(y),
                    static_cast<size_t>(w) * 4u);
    }
    buffer.clearDirty();
    return true;
}

bool loadPaintBufferFromImageBytes(TexturePaintBuffer& buffer,
                                   const uint8_t* data,
                                   std::size_t size)
{
    QImage qimg;
    if (!qimg.loadFromData(data, static_cast<int>(size)))
        return false;
    return copyQImageToPaintBuffer(buffer, qimg);
}

bool loadPaintBufferFromDiskPath(TexturePaintBuffer& buffer, const QString& path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return false;
    return copyQImageToPaintBuffer(buffer, QImage(path));
}

// CPU-side sources first — same order as MaterialEditorQML::previewUrlFromOgreTexture.
// GPU readback (convertToImage / blitToMemory) is unreliable for imported FBX textures.
bool loadPaintBufferFromNonGpuSources(TexturePaintBuffer& buffer,
                                      const Ogre::TexturePtr& texPtr,
                                      const QString& texName)
{
    if (texName.isEmpty())
        return false;

    if (texPtr) {
        const QString origin = QString::fromStdString(texPtr->getOrigin());
        if (!origin.isEmpty() && loadPaintBufferFromDiskPath(buffer, origin))
            return true;

        const QString group = QString::fromStdString(texPtr->getGroup());
        if (!group.isEmpty()) {
            if (loadPaintBufferFromDiskPath(buffer, group + QLatin1Char('/') + texName))
                return true;
            if (!origin.isEmpty()
                && loadPaintBufferFromDiskPath(buffer, group + QLatin1Char('/') + origin)) {
                return true;
            }
        }
    }

    const std::vector<uint8_t> bytes =
        EmbeddedTextureCache::retrieve(texName.toStdString());
    if (!bytes.empty()
        && loadPaintBufferFromImageBytes(buffer, bytes.data(), bytes.size())) {
        return true;
    }

    const QString baseName = QFileInfo(texName).fileName();
    if (baseName != texName) {
        const std::vector<uint8_t> baseBytes =
            EmbeddedTextureCache::retrieve(baseName.toStdString());
        if (!baseBytes.empty()
            && loadPaintBufferFromImageBytes(buffer, baseBytes.data(), baseBytes.size())) {
            return true;
        }
    }

    return loadPaintBufferFromDiskPath(buffer, texName);
}

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

    // Listen for scene-node destruction so we can drop dangling paint
    // session references before the source Entity goes away. Without
    // this, deleting a mesh while it had an active paint session or
    // a wand selection mask would crash on the next frame — the
    // mask-overlay clone holds a MeshPtr keyed off the dying entity,
    // and m_paintMeshEntity / m_sessionEntity remain pointed at
    // freed memory. Manager emits the signal BEFORE actually
    // destroying the node so the entities are still valid here.
    if (auto* mgr = Manager::getSingletonPtr()) {
        connect(mgr, &Manager::sceneNodeDestroyed, this,
            [this](Ogre::SceneNode* node) {
                if (!node) return;
                // If any attached object on the doomed node is the
                // session entity (or the overlay clone), tear down
                // the session now while the entity is still alive.
                bool touches = false;
                try {
                    const auto& objs = node->getAttachedObjects();
                    for (auto* o : objs) {
                        if (!o) continue;
                        if (o == static_cast<Ogre::MovableObject*>(m_paintMeshEntity)
                         || o == static_cast<Ogre::MovableObject*>(m_sessionEntity)
                         || o == static_cast<Ogre::MovableObject*>(m_maskOverlayEntity)) {
                            touches = true;
                            break;
                        }
                    }
                } catch (...) { touches = true; }
                if (touches) {
                    SentryReporter::addBreadcrumb("ui.action",
                        "Paint: scene node holding session entity destroyed — closing session");
                    try { closeSession(); } catch (...) {}
                }
            });
        connect(mgr, &Manager::sceneClearing, this, [this]() {
            // Hard scene reset — every entity is about to go away, so
            // unconditionally tear down the paint session.
            try { closeSession(); } catch (...) {}
        });
    }
}

TexturePaintController::~TexturePaintController()
{
    // Drop the manual objects on the scene before Ogre destructors
    // race us on shutdown. Skip the cleanup entirely if Ogre's
    // singletons are already gone — touching them post-teardown
    // segfaults at process exit.
    if (Ogre::Root::getSingletonPtr() == nullptr) {
        m_paintMesh.reset();
        m_ogreTexture.reset();
        m_originalTexture.reset();
        m_ringNode = nullptr;
        m_ringObj = nullptr;
        m_paintMeshEntity = nullptr;
        m_sessionEntity = nullptr;
        return;
    }
    try {
        closeSession();
    } catch (...) {
        // Best-effort shutdown.
    }
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
        if (auto* e = activeEntity()) {
            ensureEditableMesh(e);
            // Pre-create the paint session for texture target so the
            // preview thumbnail populates immediately and the first
            // stroke doesn't have to do the heavy setup work.
            // (Skip for vertex target — no texture session needed.)
            if (m_target == TargetTexture && !hasActiveSession())
                ensurePaintableTexture(1024);
        }
        refreshSlots();
    }
    if (!enabled) {
        if (m_strokeActive) endStroke();
        // Disabling paint also tears down any active session so the
        // model's original textures snap back into the render.
        if (hasActiveSession()) closeSession();
    }
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

void TexturePaintController::setPaintTarget(int target)
{
    PaintTarget t = static_cast<PaintTarget>(target);
    if (t == m_target) return;

    // Abort any active stroke first — switching target mid-stroke
    // crashes because beginStroke captured one set of buffers and
    // updateStroke would write into the other.
    if (m_strokeActive) {
        try { endStroke(); } catch (...) {}
    }
    // Tear down the texture-paint session when leaving texture target.
    // The session owns a GPU texture, rebind state, and an EditableMesh
    // built for the texture-paint flow; leaving any of that in place
    // when target=Vertex was the source of the "switch crashes app"
    // bug — the next vertex stroke called into half-initialized state.
    if (m_target == TargetTexture && t == TargetVertex && hasActiveSession()) {
        try { closeSession(); } catch (...) {}
    }
    m_target = t;
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Paint target = %1").arg(target == TargetVertex ? "vertex" : "texture"));
    emit paintTargetChanged();
    emit sessionChanged();
    emit smartSelectChanged();  // the mask UI is texture-only
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

double TexturePaintController::texturePaintRadiusUV() const
{
    // Same mapping the texture-paint dispatch uses (see applyBrushAt)
    // — divide the mesh-local radius by the mesh bounding-box half-
    // size. When no paint mesh is available yet (no session) the raw
    // mesh-local value is returned; the QML overlay treats anything
    // > 0.5 as "clamp visually".
    const double raw = texturePaintRadius();
    if (!m_paintMesh) return raw;
    const auto bbox = m_paintMesh->calculateBounds();
    if (!bbox.isFinite()) return raw;
    const float meshExtent = bbox.getSize().length() * 0.5f;
    if (meshExtent <= 0.0f) return raw;
    double uv = raw / static_cast<double>(meshExtent);
    if (uv < 0.005) uv = 0.005;
    if (uv > 1.0)  uv = 1.0;
    return uv;
}

int TexturePaintController::brushShape() const
{
    auto* em = EditModeController::instance();
    return em ? em->vertexPaintShape() : 0;
}

QColor TexturePaintController::bgPaintColor() const
{
    auto* em = EditModeController::instance();
    return em ? em->vertexPaintBackgroundColor() : QColor(255, 255, 255);
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

    if (m_sessionEntity == entity && m_buffer.width() > 0 && !m_textureName.isEmpty()) {
        // Active session for this entity — make sure m_paintMesh is
        // valid (it could have been torn down by a previous
        // closeSession that fired after the user's last stroke).
        if (!ensureEditableMesh(entity)) {
            emit sessionChanged();
            return false;
        }
        return true;
    }

    // Reset any prior session first — closeSession() clears m_paintMesh,
    // so we have to rebuild the EditableMesh AFTER this call. Doing it
    // before would leave m_paintMesh null on return, breaking every
    // hit-test (findMeshPointForUV walks m_paintMesh's submeshes).
    closeSession();
    m_sessionEntity = entity;

    if (!ensureEditableMesh(entity)) {
        // No mesh data → can't UV-hit-test → can't paint.
        m_sessionEntity = nullptr;
        emit sessionChanged();
        return false;
    }

    auto* tu = findOrCreateActiveTextureUnit(entity);
    if (!tu) {
        emit sessionChanged();
        return false;
    }

    QString existingTex = QString::fromStdString(tu->getTextureName());
    bool loadedExisting = false;
    QString loadError;
    // Track the original texture handle (not just its name) so we
    // can paint directly into it via blitFromMemory and skip the
    // TUS rebind entirely. This is the most reliable path on macOS
    // Metal where "missing texture" fallback gives yellow and
    // TU_DYNAMIC manual textures sometimes don't get uploaded.
    Ogre::TexturePtr originalTex;
    if (!existingTex.isEmpty()) {
        try {
            originalTex = findTextureAcrossGroups(existingTex.toStdString());
        } catch (...) {}
    }
    m_originalTexture = originalTex;
    m_originalTextureName = existingTex;
    if (!existingTex.isEmpty()) {
        // Try multiple strategies because imported PBR textures can
        // come from inline FBX embeds (no disk file), legacy on-disk
        // files, or auto-generated render targets. Each strategy
        // succeeds for a different source.
        Ogre::TexturePtr existing = originalTex;
        if (!existing) {
            try {
                existing = findTextureAcrossGroups(existingTex.toStdString());
            } catch (...) {}
        }

        // 0. CPU-side: embedded FBX bytes, on-disk origin, resource-group path.
        if (loadPaintBufferFromNonGpuSources(m_buffer, existing, existingTex)) {
            loadedExisting = true;
            loadError.clear();
        }

        // 1. TextureManager → convertToImage (works when Ogre keeps
        //    pixels in an Image buffer beside the GPU upload).
        if (!loadedExisting && existing) {
            try {
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
                loadError = QStringLiteral("convertToImage exception");
            }
        } else if (!loadedExisting && !existing) {
            loadError = QStringLiteral("texture not found in TextureManager");
        }

        // 2. Read from GPU directly via blitToMemory. Works when the
        //    texture is on the GPU but its source Image buffer was
        //    discarded post-upload (common for static textures).
        //    Reading in the texture's native format and letting Ogre
        //    convert is more reliable than asking for RGBA up front —
        //    some Metal/GL drivers refuse the format mismatch.
        if (!loadedExisting && existing) {
            try {
                if (!existing->isLoaded()) existing->load();
                const int w = static_cast<int>(existing->getWidth());
                const int h = static_cast<int>(existing->getHeight());
                auto pixbuf = existing->getBuffer();
                if (w > 0 && h > 0 && pixbuf) {
                    const Ogre::PixelFormat srcFmt = existing->getFormat();
                    SentryReporter::addBreadcrumb("ui.action",
                        QStringLiteral("Texture paint: blit native fmt=%1 size=%2x%3")
                            .arg(static_cast<int>(srcFmt)).arg(w).arg(h));
                    const size_t srcBytes = Ogre::PixelUtil::getMemorySize(w, h, 1, srcFmt);
                    std::vector<uint8_t> srcBuf(srcBytes);
                    Ogre::PixelBox srcPb(w, h, 1, srcFmt, srcBuf.data());
                    pixbuf->blitToMemory(srcPb);
                    m_buffer.resize(w, h);
                    Ogre::PixelBox dstPb(w, h, 1, Ogre::PF_BYTE_RGBA,
                                         m_buffer.data().data());
                    Ogre::PixelUtil::bulkPixelConversion(srcPb, dstPb);
                    m_buffer.clearDirty();
                    loadedExisting = true;
                    loadError.clear();
                }
            } catch (const Ogre::Exception& e) {
                loadError = QStringLiteral("blit-native: ") + QString::fromStdString(e.getDescription());
            } catch (...) {
                loadError = QStringLiteral("blit-native: unknown exception");
            }
        }

        // 3. Ogre::Image::load — works when the texture name is also
        //    a filename in a registered resource location.
        if (!loadedExisting) {
            try {
                Ogre::Image img;
                img.load(existingTex.toStdString(),
                         Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
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
                    loadError.clear();
                }
            } catch (const Ogre::Exception& e) {
                loadError = QStringLiteral("image-load: ") + QString::fromStdString(e.getDescription());
            } catch (...) {
                loadError = QStringLiteral("image-load: unknown");
            }
        }

        // 4. QImage as a raw file path. Handles textures whose name
        //    is a relative or absolute filesystem path (some assets
        //    keep their texture name == on-disk path).
        if (!loadedExisting) {
            try {
                QImage qimg(existingTex);
                if (!qimg.isNull()) {
                    qimg = qimg.convertToFormat(QImage::Format_RGBA8888);
                    const int w = qimg.width();
                    const int h = qimg.height();
                    m_buffer.resize(w, h);
                    for (int y = 0; y < h; ++y) {
                        std::memcpy(m_buffer.data().data() + static_cast<size_t>(y) * w * 4u,
                                    qimg.constScanLine(y),
                                    static_cast<size_t>(w) * 4u);
                    }
                    m_buffer.clearDirty();
                    loadedExisting = true;
                    loadError.clear();
                }
            } catch (...) {
                // All four failed.
            }
        }
    }

    if (!loadedExisting) {
        const int res = std::max(16, resolution);
        m_buffer.resize(res, res);
        m_buffer.clear(Ogre::ColourValue::White);
        m_buffer.clearDirty();
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Texture paint: starting from blank %1×%1 (existing tex='%2', err='%3')")
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
    // Rebind eagerly on session create. The deferred / lazy rebind
    // approach was racing with the mouse-move event stack and
    // crashing mid-stroke. Doing it at session-create time happens
    // BEFORE the first stroke fires, so there's no re-entrant render
    // hazard.
    // Create the GPU texture but DEFER the material rebind. Doing
    // mat->compile()/mat->reload() on the same call stack as the
    // mouse-press event raced with Ogre's render thread and
    // segfaulted mid-stroke. The rebind happens on the next
    // event-loop tick via the deferred-rebind path in
    // doFlushDirtyToOgre.
    if (!createOgreTextureFromBuffer(entity, hint, /*rebindToModel=*/false)) {
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

    // The selection mask is paired 1:1 with the paint buffer. Re-size
    // on every session so smartSelect's per-pixel indexing matches.
    m_mask.resize(m_buffer.width(), m_buffer.height());
    m_maskOverlayUri.clear();

    refreshPreviewUri();
    if (m_uvOverlayVisible) refreshUvOverlay();
    emit sessionChanged();
    emit smartSelectChanged();
    return true;
}

bool TexturePaintController::createOgreTextureFromBuffer(Ogre::Entity* entity,
                                                          const QString& nameHint,
                                                          bool rebindToModel)
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
            // Plain TU_DYNAMIC: write-only with no discard hint. The
            // _DISCARDABLE variant can drop initial content on some
            // drivers — bad for us since the initial blit IS the
            // user's "current pixels" baseline.
            Ogre::TU_DYNAMIC_WRITE_ONLY);
        if (!m_ogreTexture) return false;
        // Initial upload
        auto pixbuf = m_ogreTexture->getBuffer();
        if (!pixbuf) return false;
        Ogre::PixelBox pb(m_buffer.width(), m_buffer.height(), 1,
                          Ogre::PF_BYTE_RGBA, m_buffer.data().data());
        pixbuf->blitFromMemory(pb);
        m_textureName = QString::fromStdString(texName);

        if (rebindToModel)
            rebindEntityDiffuseToPaintTexture(entity);
        else
            SentryReporter::addBreadcrumb("ui.action",
                QStringLiteral("Texture paint: GPU texture created, deferred rebind"));
        return true;
    } catch (const Ogre::Exception&) {
        return false;
    } catch (...) {
        return false;
    }
}

void TexturePaintController::rebindEntityDiffuseToPaintTexture(Ogre::Entity* entity)
{
    if (!entity || m_textureName.isEmpty()) return;
    // Capture the original texture name from the user's active slot
    // (or the first diffuse-like TUS as fallback).
    auto* tu = findOrCreateActiveTextureUnit(entity);
    const std::string originalTexName = tu ? tu->getTextureName() : "";
    const std::string texName = m_textureName.toStdString();

    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Texture paint: rebind base = '%1' → '%2'")
            .arg(QString::fromStdString(originalTexName))
            .arg(QString::fromStdString(texName)));

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
                    // Bind the TexturePtr directly. setTextureName
                    // does name resolution that can pick the wrong
                    // group on macOS/Metal. Direct binding never
                    // mis-resolves.
                    if (m_ogreTexture)
                        tusN->setTexture(m_ogreTexture);
                    else
                        tusN->setTextureName(texName);
                    changed = true;
                }
            }
        }
        if (changed) touched.insert(mat.get());
    }
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Texture paint: bound %1 TUSes").arg(m_boundSlots.size()));

    for (auto* mat : touched) {
        try { mat->compile(); mat->reload(); }
        catch (const Ogre::Exception&) {}
    }
}

void TexturePaintController::flushDirtyToOgre()
{
    const auto& dirty = m_buffer.dirtyRect();
    if (dirty.empty()) return;

    // Debounce the GPU upload. Mouse-move fires at 100+ Hz; the
    // full-buffer blit is ~4 MB at 1024² and CHUGS at that rate.
    // Schedule a single coalesced flush per ~16 ms (one render
    // tick). The dirty rect keeps accumulating in the meantime, so
    // no pixels are lost — just batched.
    if (m_gpuFlushScheduled) return;
    m_gpuFlushScheduled = true;
    QTimer::singleShot(16, this, [this]() {
        m_gpuFlushScheduled = false;
        if (m_buffer.dirtyRect().empty()) return;
        doFlushDirtyToOgre();
    });
}

void TexturePaintController::doFlushDirtyToOgre()
{
    const auto& dirty = m_buffer.dirtyRect();
    if (dirty.empty()) return;

    // Preferred path: paint directly INTO the model's original
    // texture. No rebind needed — the existing material binding
    // continues to work, and the paint just modifies pixels in place.
    // Re-resolve the handle each flush so a reloaded material
    // doesn't dangle.
    if (!m_originalTextureName.isEmpty()) {
        try {
            auto fresh = Ogre::TextureManager::getSingleton().getByName(
                m_originalTextureName.toStdString(),
                Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
            if (fresh) m_originalTexture = fresh;
        } catch (...) {}
    }
    // Skip the in-place path once we've rebound the model to the
    // manual paint texture — the original is no longer what the
    // renderer samples, so blitting to it would be invisible.
    if (m_boundSlots.empty() && m_originalTexture) {
        try {
            auto pixbuf = m_originalTexture->getBuffer();
            if (pixbuf) {
                const int W = m_buffer.width();
                const int rectW = dirty.width();
                const int rectH = dirty.height();
                // Blit in the texture's NATIVE format. blitFromMemory
                // does internal conversion if the source PixelBox
                // format differs, but on some Metal backends the
                // conversion silently produces no upload — so we
                // convert our RGBA8 source to the texture's format
                // up front and submit it raw.
                const Ogre::PixelFormat dstFmt = m_originalTexture->getFormat();
                // Only handle plain uncompressed formats in-place.
                // Compressed formats (DXT/BC) need real CPU encoders
                // which can crash bulkPixelConversion on Metal.
                const bool plainFmt =
                    dstFmt == Ogre::PF_BYTE_RGBA
                 || dstFmt == Ogre::PF_BYTE_RGB
                 || dstFmt == Ogre::PF_BYTE_BGRA
                 || dstFmt == Ogre::PF_BYTE_BGR
                 || dstFmt == Ogre::PF_A8R8G8B8
                 || dstFmt == Ogre::PF_R8G8B8A8
                 || dstFmt == Ogre::PF_A8B8G8R8
                 || dstFmt == Ogre::PF_X8R8G8B8
                 || dstFmt == Ogre::PF_R8G8B8;
                if (!plainFmt) {
                    SentryReporter::addBreadcrumb("ui.action",
                        QStringLiteral("Texture paint: in-place skipped — compressed format %1")
                            .arg(static_cast<int>(dstFmt)));
                    m_originalTexture.reset();
                    // Fall through to manual-texture path.
                } else {
                    std::vector<uint8_t> srcRow(static_cast<size_t>(rectW) * static_cast<size_t>(rectH) * 4u);
                    const auto& src = m_buffer.data();
                    for (int row = 0; row < rectH; ++row) {
                        const size_t srcOff = (static_cast<size_t>(dirty.y0 + row) * static_cast<size_t>(W)
                                               + static_cast<size_t>(dirty.x0)) * 4u;
                        const size_t dstOff = static_cast<size_t>(row) * static_cast<size_t>(rectW) * 4u;
                        std::memcpy(srcRow.data() + dstOff, src.data() + srcOff,
                                    static_cast<size_t>(rectW) * 4u);
                    }
                    Ogre::PixelBox srcRgba(rectW, rectH, 1, Ogre::PF_BYTE_RGBA, srcRow.data());
                    if (dstFmt == Ogre::PF_BYTE_RGBA) {
                        Ogre::Box dst(dirty.x0, dirty.y0, dirty.x1, dirty.y1);
                        pixbuf->blitFromMemory(srcRgba, dst);
                    } else {
                        const size_t dstBytes = Ogre::PixelUtil::getMemorySize(rectW, rectH, 1, dstFmt);
                        std::vector<uint8_t> slice(dstBytes);
                        Ogre::PixelBox dstPb(rectW, rectH, 1, dstFmt, slice.data());
                        Ogre::PixelUtil::bulkPixelConversion(srcRgba, dstPb);
                        Ogre::Box dst(dirty.x0, dirty.y0, dirty.x1, dirty.y1);
                        pixbuf->blitFromMemory(dstPb, dst);
                    }
                    m_useOriginalTexture = true;
                    if (!m_loggedInPlaceBlit) {
                        m_loggedInPlaceBlit = true;
                        SentryReporter::addBreadcrumb("ui.action",
                            QStringLiteral("Texture paint: in-place blit fmt=%1 size=%2x%3")
                                .arg(static_cast<int>(dstFmt))
                                .arg(m_originalTexture->getWidth())
                                .arg(m_originalTexture->getHeight()));
                    }
                    m_buffer.clearDirty();
                    if (!m_previewRefreshScheduled) {
                        m_previewRefreshScheduled = true;
                        QTimer::singleShot(60, this, [this]() {
                            m_previewRefreshScheduled = false;
                            refreshPreviewUri();
                        });
                    }
                    return;
                }
            }
        } catch (const Ogre::Exception& e) {
            SentryReporter::addBreadcrumb("ui.action",
                QStringLiteral("Texture paint: in-place blit FAILED → fallback to manual texture (%1)")
                    .arg(QString::fromStdString(e.getDescription())));
            // Fall through to manual-texture path. Clear the
            // original-texture handle so we don't keep trying.
            m_originalTexture.reset();
        } catch (...) {
            m_originalTexture.reset();
        }
    }

    if (!m_ogreTexture) return;

    // Fallback path: the model's diffuse TUSes are rebound to our
    // manual paint texture on first flush. Defer the rebind via a
    // singleShot timer so material compile/reload doesn't run on
    // the same call stack as the mouse-move event — that was
    // racing against the active stroke and crashing.
    if (m_boundSlots.empty() && m_paintMeshEntity && !m_rebindScheduled) {
        m_rebindScheduled = true;
        Ogre::Entity* ent = m_paintMeshEntity;
        QTimer::singleShot(0, this, [this, ent]() {
            m_rebindScheduled = false;
            // The captured Ogre::Entity* could have been destroyed by the
            // time this fires (entity removed, mesh reimport, selection
            // change closing the session). Validate it's still both the
            // active paint target AND a live entity SelectionSet knows
            // about before dereferencing.
            if (m_paintMeshEntity != ent || !m_boundSlots.empty()) return;
            auto* sel = SelectionSet::getSingleton();
            if (!sel || !sel->contains(ent)) return;
            rebindEntityDiffuseToPaintTexture(ent);
        });
    }
    try {
        auto buf = m_ogreTexture->getBuffer();
        if (!buf) return;
        const int W = m_buffer.width();
        const int H = m_buffer.height();
        // Upload the FULL buffer each flush. Sub-rect blits via
        // blitFromMemory are unreliable on macOS Metal — sometimes
        // they don't end up visible. Full-frame upload is heavier
        // but always lands. (At 1024² that's ~4 MB per stroke; the
        // debounce on previewDataUri amortises CPU cost separately.)
        //
        // Copy m_buffer.data() into a transient std::vector first.
        // Ogre's Metal backend can queue the blit asynchronously;
        // pointing the PixelBox at our live buffer caused
        // use-after-free crashes when the next stroke modified
        // pixels before the GPU finished reading.
        std::vector<uint8_t> uploadCopy(m_buffer.data().begin(), m_buffer.data().end());
        Ogre::PixelBox pb(W, H, 1, Ogre::PF_BYTE_RGBA, uploadCopy.data());
        buf->blitFromMemory(pb);
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
    if (!m_paintEnabled || m_strokeActive) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Texture paint: beginStroke skipped (enabled=%1 active=%2)")
                .arg(m_paintEnabled).arg(m_strokeActive));
        return false;
    }
    if (m_target == TargetVertex) {
        // Vertex paint just needs the EditableMesh built — no GPU
        // texture, no rebind. Build it now if it isn't ready.
        if (auto* e = activeEntity())
            ensureEditableMesh(e);
        if (!m_paintMesh || !m_paintMeshEntity) {
            SentryReporter::addBreadcrumb("ui.action",
                "Vertex paint: beginStroke aborted — no mesh");
            return false;
        }
        m_paintMesh->ensureVertexColorBuffers(m_paintMeshEntity);
    } else {
        // Tear down a stale session if the selection moved to a
        // different entity since the session was created. Without
        // this, strokes hit-test against the new mesh's geometry but
        // write into the old mesh's texture/session state. Codex P1.
        auto* curEntity = activeEntity();
        if (hasActiveSession() && m_sessionEntity && curEntity
            && m_sessionEntity != curEntity) {
            SentryReporter::addBreadcrumb("ui.action",
                "Texture paint: selection changed — rebuilding session for new entity");
            closeSession();
        }
        if (!hasActiveSession()) {
            if (!ensurePaintableTexture(m_buffer.width() > 0 ? m_buffer.width() : 1024)) {
                SentryReporter::addBreadcrumb("ui.action",
                    "Texture paint: beginStroke aborted — no session could be created");
                return false;
            }
        }
    }
    m_strokeActive = true;
    m_strokeJustBegan = true;
    m_smudgeHavePrev = false;
    m_strokePreSnapshot = snapshotPixels();
    m_wandStrokeActive = false;
    m_wandStartScreenPos = screenPos;
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Paint stroke begin (target=%1 tool=%2 radius=%3 strength=%4 color=%5)")
            .arg(m_target == TargetVertex ? "vertex" : "texture")
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

    // Wand drag-to-scrub: once the smart-select tool has seeded the
    // mask at press time, every subsequent move re-runs the select
    // at the same UV seed with the tolerance derived from horizontal
    // mouse displacement. This lets the user adjust the selection
    // size live without lifting the mouse or touching a separate UI
    // control.
    if (m_tool == ToolSmartSelect && m_wandStrokeActive) {
        if (widget) {
            int vw = 0, vh = 0;
            widget->pixelSizeForCameraPicking(vw, vh);
            const int viewportW = vw > 0 ? vw : 800;
            // Scale: full tolerance range across one viewport width.
            // That's intuitive — drag from middle to the right edge of
            // the viewport ≈ 50% tolerance jump.
            const double dx = static_cast<double>(screenPos.x() - m_wandStartScreenPos.x());
            const double t = std::clamp(m_wandStartTolerance + dx / static_cast<double>(viewportW),
                                        0.0, 1.0);
            if (std::abs(t - m_smartSelectTolerance) > 1e-4) {
                m_smartSelectTolerance = t;
                emit smartSelectChanged();
                smartSelectAtUV(static_cast<double>(m_wandSeedUV.x),
                                static_cast<double>(m_wandSeedUV.y),
                                /*mode=*/0);
            }
        }
        return;
    }

    if (m_target == TargetVertex) {
        // Vertex paint: get local-space hit point and apply the
        // vertex-color brush directly on m_paintMesh.
        if (!m_paintMesh || !m_paintMeshEntity) return;
        Ogre::Vector3 localPos, localNormal;
        if (!hitTestLocalPoint(widget, screenPos, localPos, localNormal)) return;
        drawHoverRingAt(localPos, localNormal);
        const QColor c = texturePaintColor();
        const Ogre::ColourValue paint(c.redF(), c.greenF(), c.blueF(), c.alphaF());
        const auto* em = EditModeController::instance();
        const bool square = em && em->vertexPaintShape() == EditModeController::ShapeSquare;
        const bool changed = EditModeController::applyVertexColorBrush(
            *m_paintMesh, localPos,
            static_cast<float>(texturePaintRadius()),
            paint,
            static_cast<float>(texturePaintStrength()),
            static_cast<float>(texturePaintFalloff()),
            square);
        if (changed)
            m_paintMesh->commitVertexColorsToEntity(m_paintMeshEntity);
        return;
    }

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
    // Shape is sourced from EditModeController (the canonical brush
    // settings owner). Default Round = circular falloff; Square is
    // an axis-aligned constant-strength rectangle for pixel-art
    // style hard edges.
    const auto* em = EditModeController::instance();
    const TexturePaintBuffer::BrushShape shape =
        (em && em->vertexPaintShape() == EditModeController::ShapeSquare)
            ? TexturePaintBuffer::BrushShape::Square
            : TexturePaintBuffer::BrushShape::Round;

    switch (m_tool) {
    case ToolPaint: {
        const QColor c = texturePaintColor();
        const Ogre::ColourValue paint(c.redF(), c.greenF(), c.blueF(), c.alphaF());
        return m_buffer.paintBrush(uv, radius, paint, strength, falloff, shape) > 0;
    }
    case ToolErase: {
        // Erase = paint with the user-chosen background color. The BG
        // color is part of the FG/BG color pair (Photoshop / GIMP /
        // Krita model). When BG is fully transparent (alpha 0) this
        // matches the old "erase to transparent" behaviour; otherwise
        // it lays down a solid replacement color — much more useful
        // for actually masking out parts of a texture.
        const QColor bg = bgPaintColor();
        const Ogre::ColourValue eraseTo(
            static_cast<float>(bg.redF()),
            static_cast<float>(bg.greenF()),
            static_cast<float>(bg.blueF()),
            static_cast<float>(bg.alphaF()));
        return m_buffer.paintBrush(uv, radius, eraseTo, strength, falloff, shape) > 0;
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
    case ToolSmartSelect: {
        // Press = seed the selection at uv. Subsequent moves don't
        // re-seed — instead they nudge the tolerance and re-run the
        // select at the press seed. The on-move tolerance update
        // happens in updateStroke / updateStrokeUV because we need
        // the screen / UV delta to compute the scrub; this case
        // handles the press-time seed only.
        //
        // Don't clobber the user-configured tolerance — the default
        // lives in the member initializer in the header. Just
        // remember it as the wand-stroke baseline so drag deltas
        // are added relative to where the user started.
        if (!m_strokeJustBegan) return false;
        m_strokeJustBegan = false;
        m_wandStrokeActive = true;
        m_wandSeedUV = uv;
        m_wandStartTolerance = m_smartSelectTolerance;
        smartSelectAtUV(static_cast<double>(uv.x), static_cast<double>(uv.y), /*mode=*/0);
        return false;  // smart-select doesn't dirty pixels
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
    // Wand-drag stroke never dirties pixels, so the post-snapshot
    // diff below would be a no-op; just clear the wand flags and bail.
    if (m_wandStrokeActive) {
        m_wandStrokeActive = false;
        m_strokePreSnapshot.clear();
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Wand stroke end (final tolerance=%1)")
                .arg(m_smartSelectTolerance, 0, 'f', 3));
        return;
    }
    // Ensure any pending debounced GPU upload runs immediately so
    // the final stroke pixels are visible before the user releases.
    if (!m_buffer.dirtyRect().empty())
        doFlushDirtyToOgre();
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
    // Cache the painted pixels in-memory only. The user's original
    // texture file on disk is NEVER touched during a stroke — they
    // would lose paint on Cmd-Z, but they would also lose their
    // unmodified asset if they were just experimenting. The
    // EmbeddedTextureCache feeds the FBX exporter (and the in-engine
    // re-bind), so an explicit Save / Export still picks up the
    // painted texture. To persist to disk the user must invoke "Save
    // to Original" or "Save…" / export. See bakeToOriginalFile().
    updateEmbeddedTextureCache();
}

void TexturePaintController::updateEmbeddedTextureCache()
{
    if (m_target != TargetTexture) return;
    if (m_originalTextureName.isEmpty()) return;
    if (m_buffer.width() <= 0 || m_buffer.height() <= 0) return;
    try {
        QImage img(const_cast<uchar*>(m_buffer.data().data()),
                   m_buffer.width(), m_buffer.height(),
                   m_buffer.width() * 4, QImage::Format_RGBA8888);
        QByteArray bytes;
        QBuffer qbuf(&bytes);
        qbuf.open(QIODevice::WriteOnly);
        if (img.save(&qbuf, "PNG")) {
            std::vector<uint8_t> v(bytes.begin(), bytes.end());
            EmbeddedTextureCache::store(
                m_originalTextureName.toStdString(), v);
        }
    } catch (...) {}
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
        // Re-create the Ogre texture at the new resolution. The user
        // explicitly loaded a new image — rebind immediately so they
        // see it on the model.
        m_ogreTexture.reset();
        if (!createOgreTextureFromBuffer(entity, hint, /*rebindToModel=*/true)) return false;
    }
    // Refresh the 2D preview panel — the buffer just got rewritten
    // with the loaded image's pixels.
    refreshPreviewUri();
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

QString TexturePaintController::bakeToOriginalFile()
{
    if (m_originalTextureName.isEmpty()) return QString();
    if (m_buffer.width() <= 0 || m_buffer.height() <= 0) return QString();

    // Resolve the on-disk path. The texture name might already be a
    // full filename; Ogre's resource manager has it indexed under
    // each registered FileSystem location.
    QString diskPath;
    auto& rgm = Ogre::ResourceGroupManager::getSingleton();
    const Ogre::String texName = m_originalTextureName.toStdString();
    // Try every group's listResourceLocations.
    try {
        for (const auto& grp : rgm.getResourceGroups()) {
            auto locs = rgm.listResourceLocations(grp);
            for (const auto& loc : *locs) {
                QDir d(QString::fromStdString(loc));
                const QString candidate = d.filePath(m_originalTextureName);
                if (QFileInfo(candidate).exists()) {
                    diskPath = candidate;
                    break;
                }
            }
            if (!diskPath.isEmpty()) break;
        }
    } catch (...) {}

    if (diskPath.isEmpty()) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Bake to original: no disk file for texture '%1' (embedded?)")
                .arg(m_originalTextureName));
        return QString();
    }
    if (!m_buffer.save(diskPath.toStdString())) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Bake to original: save FAILED at %1").arg(diskPath));
        return QString();
    }
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Bake to original: wrote %1").arg(diskPath));
    return diskPath;
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
    // Bake is an explicit user action — rebind immediately so the
    // baked result appears on the model.
    createOgreTextureFromBuffer(entity, hint, /*rebindToModel=*/true);

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
    // Undo / redo replaces the buffer entirely — the cache that
    // backs exports needs to follow, otherwise FBX export sees the
    // pixels from BEFORE the last mutation.
    updateEmbeddedTextureCache();
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
    // Source scene manager from the global singleton — going through
    // m_paintMeshEntity->_getManager() is unsafe if the entity was
    // cascade-destroyed (mesh removed while paint was active).
    {
        auto* mgr = Manager::getSingletonPtr();
        auto* sceneMgr = mgr ? mgr->getSceneMgr() : nullptr;
        if (sceneMgr) {
            try {
                if (m_ringNode) {
                    m_ringNode->detachAllObjects();
                    sceneMgr->getRootSceneNode()->removeChild(m_ringNode);
                    sceneMgr->destroySceneNode(m_ringNode);
                }
                if (m_ringObj)
                    sceneMgr->destroyManualObject(m_ringObj);
            } catch (...) {}
        }
        m_ringNode = nullptr;
        m_ringObj = nullptr;
    }
    m_paintMesh.reset();
    m_paintMeshEntity = nullptr;
    m_buffer = TexturePaintBuffer();
    m_textureName.clear();
    m_originalTexture.reset();
    m_originalTextureName.clear();
    m_useOriginalTexture = false;
    m_loggedInPlaceBlit = false;
    m_rebindScheduled = false;
    m_gpuFlushScheduled = false;
    m_sessionEntity = nullptr;
    m_strokePreSnapshot.clear();
    if (!m_uvOverlayUri.isEmpty()) {
        m_uvOverlayUri.clear();
        emit uvOverlayChanged();
    }
    // Tear down smart-select state too — a stale mask sized for the
    // previous buffer would crash smartSelectAtUV. Also drop the on-
    // mesh wand overlay (it pointed at the old entity).
    m_mask = PaintSelectionMask();
    destroyMeshMaskOverlay();
    if (!m_maskOverlayUri.isEmpty()) {
        m_maskOverlayUri.clear();
        emit smartSelectChanged();
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
    m_wandStrokeActive = false;
    // Re-use the screen-pos field to stash press UV (u in pixel-ish
    // units). updateStrokeUV reads the delta from the current u.
    m_wandStartScreenPos = QPoint(static_cast<int>(u * 10000.0), 0);
    emit hoveredUVChanged(u, v);
    updateStrokeUV(u, v);
    return true;
}

void TexturePaintController::updateStrokeUV(double u, double v)
{
    if (!m_strokeActive || !m_paintEnabled) return;
    const Ogre::Vector2 uv(static_cast<float>(u), static_cast<float>(v));
    emit hoveredUVChanged(u, v);

    // Wand drag-to-scrub from the 2D thumbnail. Horizontal UV delta
    // maps directly to a tolerance delta: 0..1 UV span = full
    // tolerance range, so the user can dial in coverage by sliding
    // toward / away from the seed pixel.
    if (m_tool == ToolSmartSelect && m_wandStrokeActive) {
        const double pressU = static_cast<double>(m_wandStartScreenPos.x()) / 10000.0;
        const double du = u - pressU;
        const double t = std::clamp(m_wandStartTolerance + du, 0.0, 1.0);
        if (std::abs(t - m_smartSelectTolerance) > 1e-4) {
            m_smartSelectTolerance = t;
            emit smartSelectChanged();
            smartSelectAtUV(static_cast<double>(m_wandSeedUV.x),
                            static_cast<double>(m_wandSeedUV.y),
                            /*mode=*/0);
        }
        return;
    }
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
    // Refresh is pure metadata — it must not create a paint session
    // as a side effect. Otherwise toggling the brush button (which
    // calls refreshSlots) silently rebinds the model's diffuse TUS
    // and the user sees the model "go textureless". Sessions are
    // created lazily inside beginStroke() instead.
    //
    // But we DO eagerly build the EditableMesh for the selected
    // entity so hover/brush-ring queries work without an active
    // session. The EditableMesh is just a CPU mirror of mesh data;
    // it doesn't alter the model's render.
    if (m_paintEnabled) {
        if (auto* e = activeEntity())
            ensureEditableMesh(e);
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
    // Both the Inspector thumbnail and the detached editor window now
    // consume `fullResPreviewUrl`, which is served by
    // PaintBufferImageProvider (a QImage view of the buffer — no PNG
    // encode, no base64). The legacy `previewDataUri` PNG path is
    // retained as a property for binary compatibility but no longer
    // populated; emitting `previewChanged` keeps any external observer
    // notified without paying the encode cost on the main thread.
    if (m_buffer.width() <= 0 || m_buffer.height() <= 0) {
        if (!m_previewUri.isEmpty()) {
            m_previewUri.clear();
            emit previewChanged();
        }
    }
    ++m_fullResVersion;
    emit fullResPreviewChanged();
}

QString TexturePaintController::fullResPreviewUrl() const
{
    // The trailing `?v=N` invalidates QML's Image cache on each
    // refresh; the provider ignores it. Static `paintbuffer` host
    // is registered in main.cpp so any consumer can bind to this.
    if (m_buffer.width() <= 0 || m_buffer.height() <= 0)
        return {};
    return QStringLiteral("image://paintbuffer/current?v=%1").arg(m_fullResVersion);
}

QImage TexturePaintController::snapshotBufferImage() const
{
    if (m_buffer.width() <= 0 || m_buffer.height() <= 0) return {};
    // Copy out — Qt may hold the QImage across thread boundaries
    // and the underlying m_buffer could mutate (or get freed)
    // between the provider call and the actual GPU upload.
    QImage view(const_cast<uchar*>(m_buffer.data().data()),
                m_buffer.width(), m_buffer.height(),
                m_buffer.width() * 4, QImage::Format_RGBA8888);
    return view.copy();
}

// ---------------------------------------------------------------------------
// 3D-mesh hover ring overlay
// ---------------------------------------------------------------------------

void TexturePaintController::updateMeshHover(OgreWidget* widget, const QPoint& screenPos)
{
    if (!m_paintEnabled) { clearMeshHover(); return; }
    // Build the EditableMesh on first hover if it isn't already
    // ready. This handles the "user selected the mesh AFTER enabling
    // paint" case so the brush ring appears as soon as the cursor
    // touches the mesh.
    if (!m_paintMesh || !m_paintMeshEntity) {
        if (auto* e = activeEntity())
            ensureEditableMesh(e);
    }
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

bool TexturePaintController::hitTestLocalPoint(OgreWidget* widget, const QPoint& screenPos,
                                                Ogre::Vector3& outLocal, Ogre::Vector3& outNormal) const
{
    if (!m_paintMesh || !m_paintMeshEntity || !widget) return false;
    auto* spaceCam = widget->getSpaceCamera();
    auto* camera = spaceCam ? spaceCam->getCamera() : nullptr;
    if (!camera) return false;
    int vw = 0, vh = 0;
    widget->pixelSizeForCameraPicking(vw, vh);
    if (vw <= 0 || vh <= 0) return false;
    const Ogre::Real nx = static_cast<Ogre::Real>(screenPos.x()) / vw;
    const Ogre::Real ny = static_cast<Ogre::Real>(screenPos.y()) / vh;
    const Ogre::Ray ray = camera->getCameraToViewportRay(nx, ny);
    Ogre::SceneNode* node = m_paintMeshEntity->getParentSceneNode();
    Ogre::Affine3 worldToLocal = node ? node->_getFullTransform().inverse() : Ogre::Affine3::IDENTITY;
    Ogre::Vector3 localOrigin = worldToLocal * ray.getOrigin();
    Ogre::Vector3 localDir = worldToLocal.linear() * ray.getDirection();
    localDir.normalise();
    Ogre::Real bestT = std::numeric_limits<Ogre::Real>::infinity();
    bool found = false;
    for (const auto& sub : m_paintMesh->subMeshes()) {
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
            outLocal = localOrigin + localDir * tHit;
            Ogre::Vector3 n = e1.crossProduct(e2);
            if (!n.isZeroLength()) n.normalise();
            else n = Ogre::Vector3::UNIT_Y;
            outNormal = n;
            found = true;
        }
    }
    return found;
}

bool TexturePaintController::wouldStrokeHit(OgreWidget* widget,
                                            const QPoint& screenPos)
{
    if (!widget) return false;
    // Lazily build the paint mesh — beginStroke would do it anyway,
    // and without this the very first click on the model is treated
    // as a miss (m_paintMesh is null until the first stroke). That
    // bug masked the click-outside-clears flow because the seed
    // press never reached beginStroke.
    if (!m_paintMesh || !m_paintMeshEntity) {
        if (auto* e = activeEntity())
            ensureEditableMesh(e);
        if (!m_paintMesh || !m_paintMeshEntity) return false;
    }
    Ogre::Vector2 uv;
    return hitTestUV(screenPos, widget, uv);
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
    // The brush radius is in local mesh units (shared with vertex paint).
    // For the circular overlay we narrow it slightly (0.8x) so the ring
    // visually matches the painted footprint — a softly-falloff brush
    // doesn't quite touch the ring edge. The square overlay represents
    // the AABB cube footprint exactly, so it uses the full radius.
    const Ogre::Vector3 center = localPos + normal * 0.001f;

    const auto* em = EditModeController::instance();
    const bool square = em && em->vertexPaintShape() == EditModeController::ShapeSquare;
    if (square) {
        const float radius = static_cast<float>(texturePaintRadius());
        // Four corners of the local tangent-plane square, drawn as a
        // closed line strip. Tangent + bitangent are perpendicular by
        // construction so the corners are axis-aligned in the surface
        // frame. NB: applyVertexColorBrush uses an AABB in mesh-local
        // X/Y/Z space, not the surface frame — this overlay is a
        // close visual approximation that stays simple, not an exact
        // projection of that cube. Users want a hint of "square
        // brush", which this delivers.
        const Ogre::Vector3 p0 = center + ( tangent + bitangent) * radius;
        const Ogre::Vector3 p1 = center + (-tangent + bitangent) * radius;
        const Ogre::Vector3 p2 = center + (-tangent - bitangent) * radius;
        const Ogre::Vector3 p3 = center + ( tangent - bitangent) * radius;
        m_ringObj->begin(kMatName, Ogre::RenderOperation::OT_LINE_STRIP);
        m_ringObj->position(p0); m_ringObj->colour(ringCol);
        m_ringObj->position(p1); m_ringObj->colour(ringCol);
        m_ringObj->position(p2); m_ringObj->colour(ringCol);
        m_ringObj->position(p3); m_ringObj->colour(ringCol);
        m_ringObj->position(p0); m_ringObj->colour(ringCol);
        m_ringObj->end();
    } else {
        const float radius = static_cast<float>(texturePaintRadius()) * 0.8f;
        constexpr int kSegments = 64;
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
}

// ---------------------------------------------------------------------------
// Smart-select / selection-mask API
// ---------------------------------------------------------------------------

void TexturePaintController::setSmartSelectTolerance(double t)
{
    const double clamped = std::clamp(t, 0.0, 1.0);
    if (m_smartSelectTolerance == clamped) return;
    m_smartSelectTolerance = clamped;
    emit smartSelectChanged();
}

bool TexturePaintController::hasSelectionMask() const
{
    return !m_mask.isEmpty();
}

int TexturePaintController::selectedPixelCount() const
{
    return m_mask.selectedCount();
}

void TexturePaintController::clearSelectionMask()
{
    if (m_mask.isEmpty()) return;
    m_mask.clear();
    m_maskOverlayUri.clear();
    destroyMeshMaskOverlay();
    SentryReporter::addBreadcrumb("ui.action", "Smart select: cleared");
    emit smartSelectChanged();
}

void TexturePaintController::selectAllMask()
{
    if (!hasActiveSession()) return;
    if (m_mask.width() != m_buffer.width() || m_mask.height() != m_buffer.height())
        m_mask.resize(m_buffer.width(), m_buffer.height());
    m_mask.selectAll();
    SentryReporter::addBreadcrumb("ui.action", "Smart select: select all");
    scheduleMaskOverlayRefresh();
    emit smartSelectChanged();
}

void TexturePaintController::invertSelectionMask()
{
    if (!hasActiveSession()) return;
    if (m_mask.width() != m_buffer.width() || m_mask.height() != m_buffer.height())
        m_mask.resize(m_buffer.width(), m_buffer.height());
    m_mask.invert();
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Smart select: invert (%1 px now)").arg(m_mask.selectedCount()));
    scheduleMaskOverlayRefresh();
    emit smartSelectChanged();
}

int TexturePaintController::smartSelectAtUV(double u, double v, int mode)
{
    if (!hasActiveSession()) return 0;
    if (m_mask.width() != m_buffer.width() || m_mask.height() != m_buffer.height())
        m_mask.resize(m_buffer.width(), m_buffer.height());

    int sx = 0, sy = 0;
    m_buffer.uvToPixel(Ogre::Vector2(static_cast<float>(u), static_cast<float>(v)),
                       sx, sy);
    const auto cmode = (mode == 1) ? PaintSelectionMask::CombineMode::Add
                     : (mode == 2) ? PaintSelectionMask::CombineMode::Sub
                                   : PaintSelectionMask::CombineMode::Replace;
    const int affected = m_mask.smartSelect(m_buffer, sx, sy,
                                            static_cast<float>(m_smartSelectTolerance),
                                            cmode);
    if (affected > 0) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Smart select: %1 px (mode=%2, tol=%3)")
                .arg(affected).arg(mode).arg(m_smartSelectTolerance, 0, 'f', 2));
        scheduleMaskOverlayRefresh();
        emit smartSelectChanged();
    }
    return affected;
}

namespace {

// Apply `apply` to every pixel in `mask`. Returns count of affected
// pixels. Caller owns the before/after snapshots for undo.
template <typename F>
int applyToMaskedPixels(TexturePaintBuffer& buf, const PaintSelectionMask& mask, F&& apply)
{
    if (mask.isEmpty()) return 0;
    const int W = buf.width();
    const int H = buf.height();
    if (mask.width() != W || mask.height() != H) return 0;
    const auto& maskData = mask.data();
    auto& px = buf.data();
    const auto& bb = mask.bbox();
    int affected = 0;
    for (int y = bb.y0; y < bb.y1; ++y) {
        for (int x = bb.x0; x < bb.x1; ++x) {
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x);
            if (!maskData[i]) continue;
            const size_t off = i * 4u;
            apply(px[off + 0], px[off + 1], px[off + 2], px[off + 3]);
            ++affected;
        }
    }
    if (affected > 0)
        buf.markDirty(bb.x0, bb.y0, bb.x1, bb.y1);
    return affected;
}

} // namespace

int TexturePaintController::fillMaskWithFG()
{
    if (!hasActiveSession() || !hasSelectionMask()) return 0;
    auto before = m_buffer.data();
    const QColor c = texturePaintColor();
    const uint8_t fr = static_cast<uint8_t>(std::lround(c.redF()   * 255.0));
    const uint8_t fg = static_cast<uint8_t>(std::lround(c.greenF() * 255.0));
    const uint8_t fb = static_cast<uint8_t>(std::lround(c.blueF()  * 255.0));
    const uint8_t fa = static_cast<uint8_t>(std::lround(c.alphaF() * 255.0));
    const int affected = applyToMaskedPixels(m_buffer, m_mask,
        [&](uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
            r = fr; g = fg; b = fb; a = fa;
        });
    if (affected <= 0) return 0;
    UndoManager::getSingleton()->push(new TexturePaintMaskActionCommand(
        this, std::move(before), m_buffer.data(),
        m_buffer.width(), m_buffer.height(), m_textureName,
        QStringLiteral("Fill selection (FG)")));
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Smart select: filled %1 px with FG %2")
            .arg(affected).arg(c.name(QColor::HexRgb)));
    flushDirtyToOgre();
    updateEmbeddedTextureCache();
    return affected;
}

int TexturePaintController::fillMaskWithBG()
{
    if (!hasActiveSession() || !hasSelectionMask()) return 0;
    auto before = m_buffer.data();
    const QColor c = bgPaintColor();
    const uint8_t fr = static_cast<uint8_t>(std::lround(c.redF()   * 255.0));
    const uint8_t fg = static_cast<uint8_t>(std::lround(c.greenF() * 255.0));
    const uint8_t fb = static_cast<uint8_t>(std::lround(c.blueF()  * 255.0));
    const uint8_t fa = static_cast<uint8_t>(std::lround(c.alphaF() * 255.0));
    const int affected = applyToMaskedPixels(m_buffer, m_mask,
        [&](uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
            r = fr; g = fg; b = fb; a = fa;
        });
    if (affected <= 0) return 0;
    UndoManager::getSingleton()->push(new TexturePaintMaskActionCommand(
        this, std::move(before), m_buffer.data(),
        m_buffer.width(), m_buffer.height(), m_textureName,
        QStringLiteral("Fill selection (BG)")));
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Smart select: filled %1 px with BG %2")
            .arg(affected).arg(c.name(QColor::HexArgb)));
    flushDirtyToOgre();
    updateEmbeddedTextureCache();
    return affected;
}

int TexturePaintController::deleteMaskPixels()
{
    if (!hasActiveSession() || !hasSelectionMask()) return 0;
    auto before = m_buffer.data();
    const int affected = applyToMaskedPixels(m_buffer, m_mask,
        [](uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
            r = 0; g = 0; b = 0; a = 0;
        });
    if (affected <= 0) return 0;
    UndoManager::getSingleton()->push(new TexturePaintMaskActionCommand(
        this, std::move(before), m_buffer.data(),
        m_buffer.width(), m_buffer.height(), m_textureName,
        QStringLiteral("Delete selection")));
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Smart select: deleted %1 px").arg(affected));
    flushDirtyToOgre();
    updateEmbeddedTextureCache();
    return affected;
}

void TexturePaintController::scheduleMaskOverlayRefresh()
{
    if (m_maskOverlayRefreshScheduled) return;
    m_maskOverlayRefreshScheduled = true;
    QTimer::singleShot(60, this, [this]() {
        m_maskOverlayRefreshScheduled = false;
        refreshMaskOverlay();
    });
}

void TexturePaintController::refreshMaskOverlay()
{
    // Keep the on-mesh 3D overlay in sync with the mask. Doing it here
    // (the debounced path) covers smartSelect, selectAll, and invert
    // without each caller having to remember to refresh both layers.
    refreshMeshMaskOverlay();

    const int W = m_mask.width();
    const int H = m_mask.height();
    if (W <= 0 || H <= 0 || m_mask.isEmpty()) {
        if (!m_maskOverlayUri.isEmpty()) {
            m_maskOverlayUri.clear();
            emit smartSelectChanged();
        }
        return;
    }
    // Render the mask as a high-contrast translucent overlay: yellow tint
    // inside, black 1px outline at the boundary (any selected pixel with
    // an unselected 4-neighbor). This is the "marching ants" mock —
    // animation comes from the QML side if we add it later.
    const int previewW = std::min(W, 512);
    const int previewH = std::min(H, 512);
    const float sx = static_cast<float>(W) / previewW;
    const float sy = static_cast<float>(H) / previewH;
    // ARGB32 (not RGBA8888) so qRgba's 0xAARRGGBB packing maps to the
    // image's bytes correctly. With RGBA8888 the channels are
    // re-ordered to R,G,B,A in memory and the values written via qRgba
    // come out swizzled (yellow → light blue).
    QImage img(previewW, previewH, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    const auto& d = m_mask.data();
    auto sel = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= W || y >= H) return false;
        return d[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)] != 0;
    };
    for (int py = 0; py < previewH; ++py) {
        const int y = std::min(H - 1, static_cast<int>(py * sy));
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(py));
        for (int px = 0; px < previewW; ++px) {
            const int x = std::min(W - 1, static_cast<int>(px * sx));
            const bool inside = sel(x, y);
            if (!inside) continue;
            const bool boundary = !sel(x - 1, y) || !sel(x + 1, y)
                               || !sel(x, y - 1) || !sel(x, y + 1);
            line[px] = boundary ? qRgba(0, 0, 0, 220) : qRgba(255, 240, 0, 80);
        }
    }
    QByteArray bytes;
    QBuffer qbuf(&bytes);
    qbuf.open(QIODevice::WriteOnly);
    if (!img.save(&qbuf, "PNG")) return;
    m_maskOverlayUri = QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64());
    emit smartSelectChanged();
}

// ---------------------------------------------------------------------------
// Detached texture editor window
// ---------------------------------------------------------------------------

void TexturePaintController::openEditorWindow()
{
    if (m_editorWindow) {
        // Already open — raise / show.
        if (auto* w = qobject_cast<QQuickWindow*>(m_editorWindow)) {
            w->show();
            w->raise();
            w->requestActivate();
        }
        return;
    }
    // Use a QQmlApplicationEngine so the loaded Window registers as a
    // top-level (matches MaterialEditor's pattern). We also need to
    // re-register the PropertiesPanel singleton in this new engine so
    // the imported singleton resolves at QML load time — main.cpp's
    // registrations are per-engine in Qt 6.
    auto* engine = new QQmlApplicationEngine(this);
    const QString appDir = QCoreApplication::applicationDirPath();
    engine->addImportPath(appDir + "/qml");
    engine->addImportPath(QLibraryInfo::path(QLibraryInfo::QmlImportsPath));

    // Register the paintbuffer image provider so the editor window's
    // <Image source="image://paintbuffer/current?v=…"/> can fetch
    // the live buffer without a PNG round-trip. Engine takes
    // ownership of the provider via QQmlEngine::addImageProvider.
    engine->addImageProvider(QStringLiteral("paintbuffer"),
                             new PaintBufferImageProvider());

    qmlRegisterSingletonType<TexturePaintController>(
        "PropertiesPanel", 1, 0, "TexturePaintController",
        [](QQmlEngine* e, QJSEngine*) -> QObject* {
            return TexturePaintController::qmlInstance(e, nullptr);
        });

    bool handled = false;
    connect(engine, &QQmlApplicationEngine::objectCreated, this,
        [this, engine, &handled](QObject* obj, const QUrl&) {
            handled = true;
            if (!obj) {
                SentryReporter::addBreadcrumb("ui.action",
                    QStringLiteral("Texture editor window: QML load failed"));
                engine->deleteLater();
                return;
            }
            m_editorWindow = obj;
            if (auto* w = qobject_cast<QQuickWindow*>(obj)) {
                connect(w, &QQuickWindow::visibleChanged, this,
                    [this, w, engine](bool vis) {
                        if (vis || m_editorWindow != w) return;
                        m_editorWindow = nullptr;
                        emit editorWindowChanged();
                        engine->deleteLater();
                    });
                w->show();
                w->raise();
                w->requestActivate();
            }
            emit editorWindowChanged();
        }, Qt::DirectConnection);

    engine->load(QUrl(QStringLiteral("qrc:/PropertiesPanel/TextureEditorWindow.qml")));
    if (!handled) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Texture editor window: load() returned without objectCreated firing"));
    }
    SentryReporter::addBreadcrumb("ui.action", "Texture editor window opened");
}

void TexturePaintController::closeEditorWindow()
{
    if (!m_editorWindow) return;
    if (auto* w = qobject_cast<QQuickWindow*>(m_editorWindow)) {
        w->close();
        // visibleChanged handler will null m_editorWindow + emit.
    } else {
        m_editorWindow->deleteLater();
        m_editorWindow = nullptr;
        emit editorWindowChanged();
    }
}

// ---------------------------------------------------------------------------
// On-mesh wand-selection overlay
// ---------------------------------------------------------------------------

void TexturePaintController::refreshMeshMaskOverlay()
{
    if (!m_paintMeshEntity || m_mask.isEmpty()) {
        destroyMeshMaskOverlay();
        return;
    }
    auto* entity = m_paintMeshEntity;
    auto* sceneMgr = entity->_getManager();
    auto* parentNode = entity->getParentSceneNode();
    if (!sceneMgr || !parentNode) return;

    // (1) Build / refresh the overlay GPU texture from the mask data.
    //     Match the 2D preview's marching-ants palette so the 3D and
    //     2D views read as the same selection: yellow tint inside
    //     (255,240,0 α≈80), black outline (0,0,0 α≈220) on any
    //     selected pixel adjacent to an unselected one. Boundary
    //     test is the same 4-neighbor as refreshMaskOverlay.
    const int W = m_mask.width();
    const int H = m_mask.height();
    if (W <= 0 || H <= 0) {
        destroyMeshMaskOverlay();
        return;
    }
    std::vector<uint8_t> rgba(static_cast<size_t>(W) * static_cast<size_t>(H) * 4u, 0);
    const auto& d = m_mask.data();
    auto sel = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= W || y >= H) return false;
        return d[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)] != 0;
    };
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x);
            if (!d[i]) continue;
            const bool boundary = !sel(x - 1, y) || !sel(x + 1, y)
                               || !sel(x, y - 1) || !sel(x, y + 1);
            const size_t off = i * 4u;
            if (boundary) {
                rgba[off + 0] = 0;
                rgba[off + 1] = 0;
                rgba[off + 2] = 0;
                rgba[off + 3] = 220;
            } else {
                rgba[off + 0] = 255;
                rgba[off + 1] = 240;
                rgba[off + 2] = 0;
                rgba[off + 3] = 80;
            }
        }
    }
    const std::string texName = "QMEPaintMaskOverlay_"
        + std::to_string(reinterpret_cast<uintptr_t>(this));
    auto& texMgr = Ogre::TextureManager::getSingleton();
    if (m_maskOverlayTex && (static_cast<int>(m_maskOverlayTex->getWidth()) != W
                          || static_cast<int>(m_maskOverlayTex->getHeight()) != H)) {
        // Size changed — drop and recreate.
        try { texMgr.remove(m_maskOverlayTex); } catch (...) {}
        m_maskOverlayTex.reset();
    }
    if (!m_maskOverlayTex) {
        m_maskOverlayTex = texMgr.createManual(
            texName,
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
            Ogre::TEX_TYPE_2D, W, H, 0,
            Ogre::PF_BYTE_RGBA,
            Ogre::TU_DYNAMIC_WRITE_ONLY);
    }
    try {
        auto buf = m_maskOverlayTex->getBuffer();
        if (buf) {
            Ogre::PixelBox pb(W, H, 1, Ogre::PF_BYTE_RGBA, rgba.data());
            buf->blitFromMemory(pb);
        }
    } catch (...) {}

    // (2) Build / fetch the unlit transparent material that samples
    //     the overlay texture. One material per controller — reused
    //     across refreshes.
    if (m_maskOverlayMatName.empty()) {
        m_maskOverlayMatName = "QMEPaintMaskOverlay_Mat_"
            + std::to_string(reinterpret_cast<uintptr_t>(this));
    }
    auto& matMgr = Ogre::MaterialManager::getSingleton();
    Ogre::MaterialPtr mat = matMgr.getByName(m_maskOverlayMatName);
    if (!mat) {
        mat = matMgr.create(m_maskOverlayMatName,
            Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
        auto* tech = mat->getTechnique(0);
        auto* pass = tech->getPass(0);
        pass->setLightingEnabled(false);
        pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        pass->setDepthWriteEnabled(false);
        pass->setDepthCheckEnabled(true);
        pass->setDepthBias(1.0f, 1.0f);   // tiny z-pull-in so we sit on top
        pass->setCullingMode(Ogre::CULL_NONE);
        auto* tus = pass->createTextureUnitState(m_maskOverlayTex->getName());
        tus->setTextureFiltering(Ogre::TFO_NONE);
    } else {
        // Re-point the existing TUS at the (possibly resized) texture.
        try {
            auto* pass = mat->getTechnique(0)->getPass(0);
            if (pass->getNumTextureUnitStates() > 0) {
                pass->getTextureUnitState(0)->setTextureName(m_maskOverlayTex->getName());
            }
        } catch (...) {}
    }

    // (3) Create / refresh the duplicate Entity that draws the mesh
    //     with the overlay material. Sharing the source MeshPtr means
    //     the verts / UVs / animation match the base mesh for free.
    if (!m_maskOverlayEntity) {
        try {
            const std::string entName = "QMEPaintMaskOverlay_Ent_"
                + std::to_string(reinterpret_cast<uintptr_t>(this));
            m_maskOverlayEntity = sceneMgr->createEntity(entName, entity->getMesh()->getName());
            m_maskOverlayEntity->setMaterialName(m_maskOverlayMatName);
            m_maskOverlayEntity->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY - 1);
            m_maskOverlayEntity->setCastShadows(false);
            m_maskOverlayEntity->setQueryFlags(0);
        } catch (const Ogre::Exception& e) {
            SentryReporter::addBreadcrumb("ui.action",
                QStringLiteral("Mask overlay: createEntity failed: %1")
                    .arg(QString::fromStdString(e.getDescription())));
            return;
        }
    } else {
        // Ensure every submesh also samples the overlay material — Ogre
        // creates one sub-entity per submesh on createEntity and they
        // each get the source material unless we override.
        for (unsigned i = 0; i < m_maskOverlayEntity->getNumSubEntities(); ++i)
            m_maskOverlayEntity->getSubEntity(i)->setMaterialName(m_maskOverlayMatName);
    }
    if (!m_maskOverlayNode) {
        m_maskOverlayNode = parentNode->createChildSceneNode();
        m_maskOverlayNode->attachObject(m_maskOverlayEntity);
    }
}

void TexturePaintController::destroyMeshMaskOverlay()
{
    // Source scene manager via the global Manager — touching
    // m_maskOverlayEntity->_getManager() is unsafe when the entity
    // was already cascade-destroyed (e.g. user removed the mesh
    // while a wand selection was up). Manager's sceneNodeDestroyed
    // signal handler nulls our pointers preemptively, so by the time
    // we reach here the cleanup may be a partial no-op — guard each
    // step independently.
    auto* mgr = Manager::getSingletonPtr();
    auto* sceneMgr = mgr ? mgr->getSceneMgr() : nullptr;

    if (m_maskOverlayNode && sceneMgr) {
        try {
            m_maskOverlayNode->detachAllObjects();
            auto* parent = m_maskOverlayNode->getParentSceneNode();
            if (parent) parent->removeChild(m_maskOverlayNode);
            sceneMgr->destroySceneNode(m_maskOverlayNode);
        } catch (...) {}
    }
    m_maskOverlayNode = nullptr;

    if (m_maskOverlayEntity && sceneMgr) {
        try {
            // Only destroy if Ogre still thinks it owns this entity
            // by name — otherwise it was already cascade-destroyed.
            const std::string& n = m_maskOverlayEntity->getName();
            if (sceneMgr->hasEntity(n))
                sceneMgr->destroyEntity(m_maskOverlayEntity);
        } catch (...) {}
    }
    m_maskOverlayEntity = nullptr;

    if (m_maskOverlayTex) {
        try { Ogre::TextureManager::getSingleton().remove(m_maskOverlayTex); } catch (...) {}
        m_maskOverlayTex.reset();
    }
}
