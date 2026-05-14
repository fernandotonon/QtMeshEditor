#include "TexturePaintController.h"

#include "EditModeController.h"
#include "EditableMesh.h"
#include "OgreWidget.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "SpaceCamera.h"
#include "UndoManager.h"
#include "VertexColorBaker.h"

#include <QDir>
#include <QFileInfo>
#include <QUndoCommand>
#include <QtMath>

#include <OgreCamera.h>
#include <OgreEntity.h>
#include <OgreHardwarePixelBuffer.h>
#include <OgreImage.h>
#include <OgreMaterial.h>
#include <OgreMaterialManager.h>
#include <OgrePass.h>
#include <OgrePixelFormat.h>
#include <OgreRoot.h>
#include <OgreSceneNode.h>
#include <OgreSubEntity.h>
#include <OgreTechnique.h>
#include <OgreTextureManager.h>
#include <OgreTextureUnitState.h>

#include <algorithm>
#include <cstring>

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
}

TexturePaintController::~TexturePaintController() = default;

void TexturePaintController::setTexturePaintEnabled(bool enabled)
{
    if (m_paintEnabled == enabled) return;
    m_paintEnabled = enabled;
    if (!enabled && m_strokeActive)
        endStroke();
    SentryReporter::addBreadcrumb("ui.action",
        enabled ? "Texture paint: enabled" : "Texture paint: disabled");
    emit texturePaintChanged();
}

void TexturePaintController::setTexturePaintColor(const QColor& c)
{
    if (!c.isValid()) return;
    QColor rgb = c;
    rgb.setAlpha(255);
    if (m_color.rgba() == rgb.rgba()) return;
    m_color = rgb;
    emit texturePaintChanged();
}

void TexturePaintController::setTexturePaintColorHex(const QString& cssColor)
{
    if (cssColor.isEmpty()) return;
    QColor c(cssColor);
    if (!c.isValid()) return;
    setTexturePaintColor(c);
}

void TexturePaintController::setTexturePaintRadius(double r)
{
    if (r <= 0.0 || qFuzzyCompare(m_radiusUV, r)) return;
    m_radiusUV = r;
    emit texturePaintChanged();
}

void TexturePaintController::setTexturePaintStrength(double s)
{
    const double clamped = std::clamp(s, 0.0, 1.0);
    if (qFuzzyCompare(m_strength, clamped)) return;
    m_strength = clamped;
    emit texturePaintChanged();
}

void TexturePaintController::setTexturePaintFalloff(double f)
{
    const double clamped = std::clamp(f, 0.0, 1.0);
    if (qFuzzyCompare(m_falloff, clamped)) return;
    m_falloff = clamped;
    emit texturePaintChanged();
}

Ogre::Entity* TexturePaintController::activeEntity() const
{
    auto* edit = EditModeController::instance();
    if (!edit || !edit->isEditModeActive())
        return nullptr;
    return edit->editEntity();
}

Ogre::TextureUnitState* TexturePaintController::findOrCreateDiffuseTextureUnit(Ogre::Entity* entity)
{
    if (!entity || entity->getNumSubEntities() == 0) return nullptr;
    auto* subEnt = entity->getSubEntity(0);
    if (!subEnt) return nullptr;
    Ogre::MaterialPtr mat = subEnt->getMaterial();
    if (!mat || mat->getNumTechniques() == 0) return nullptr;
    auto* tech = mat->getTechnique(0);
    if (!tech || tech->getNumPasses() == 0) return nullptr;
    auto* pass = tech->getPass(0);
    if (!pass) return nullptr;
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

    if (m_sessionEntity == entity && m_buffer.width() > 0 && !m_textureName.isEmpty())
        return true;

    // Reset any prior session.
    closeSession();
    m_sessionEntity = entity;

    auto* tu = findOrCreateDiffuseTextureUnit(entity);
    if (!tu) {
        emit sessionChanged();
        return false;
    }

    QString existingTex = QString::fromStdString(tu->getTextureName());
    bool loadedExisting = false;
    if (!existingTex.isEmpty()) {
        auto existing = Ogre::TextureManager::getSingleton().getByName(
            existingTex.toStdString(),
            Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
        if (existing) {
            try {
                Ogre::Image img;
                existing->convertToImage(img);
                const int w = static_cast<int>(img.getWidth());
                const int h = static_cast<int>(img.getHeight());
                if (w > 0 && h > 0) {
                    m_buffer.resize(w, h);
                    // PF_BYTE_RGBA == 4 bytes/pixel
                    Ogre::PixelBox srcBox = img.getPixelBox();
                    Ogre::PixelBox dstBox(w, h, 1, Ogre::PF_BYTE_RGBA, m_buffer.data().data());
                    Ogre::PixelUtil::bulkPixelConversion(srcBox, dstBox);
                    m_buffer.clearDirty();
                    loadedExisting = true;
                }
            } catch (const Ogre::Exception&) {
                // Fall through to blank buffer.
            }
        }
    }

    if (!loadedExisting) {
        const int res = std::max(16, resolution);
        m_buffer.resize(res, res);
        m_buffer.clear(Ogre::ColourValue::White);
        m_buffer.clearDirty();
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

    emit sessionChanged();
    return true;
}

bool TexturePaintController::createOgreTextureFromBuffer(Ogre::Entity* entity, const QString& nameHint)
{
    if (!entity || m_buffer.width() <= 0 || m_buffer.height() <= 0) return false;
    auto* tu = findOrCreateDiffuseTextureUnit(entity);
    if (!tu) return false;

    const std::string texName = nameHint.toStdString();
    const std::string group = Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME;
    try {
        auto& tm = Ogre::TextureManager::getSingleton();
        if (auto existing = tm.getByName(texName, group))
            tm.remove(existing);
        m_ogreTexture = tm.createManual(
            texName, group, Ogre::TEX_TYPE_2D,
            m_buffer.width(), m_buffer.height(), 0,
            Ogre::PF_BYTE_RGBA, Ogre::TU_DYNAMIC_WRITE_ONLY);
        if (!m_ogreTexture) return false;
        // Initial upload
        auto buf = m_ogreTexture->getBuffer();
        if (!buf) return false;
        Ogre::PixelBox pb(m_buffer.width(), m_buffer.height(), 1,
                          Ogre::PF_BYTE_RGBA, m_buffer.data().data());
        buf->blitFromMemory(pb);
        tu->setTextureName(texName);
        m_textureName = QString::fromStdString(texName);
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
    } catch (const Ogre::Exception&) {
        // Best-effort — skip this flush.
    }
    m_buffer.clearDirty();
}

bool TexturePaintController::hitTestUV(const QPoint& screenPos, OgreWidget* widget, Ogre::Vector2& outUV) const
{
    auto* edit = EditModeController::instance();
    if (!edit || !edit->isEditModeActive()) return false;
    auto* mesh = edit->currentMesh();
    auto* entity = edit->editEntity();
    if (!mesh || !entity || !widget) return false;

    auto* spaceCam = widget->getSpaceCamera();
    auto* camera = spaceCam ? spaceCam->getCamera() : nullptr;
    if (!camera) return false;

    int vw = 0, vh = 0;
    widget->pixelSizeForCameraPicking(vw, vh);
    if (vw <= 0 || vh <= 0) return false;

    const int triHit = edit->hitTestFace(screenPos, camera, vw, vh);
    if (triHit < 0) return false;

    const Ogre::Real nx = static_cast<Ogre::Real>(screenPos.x()) / vw;
    const Ogre::Real ny = static_cast<Ogre::Real>(screenPos.y()) / vh;
    const Ogre::Ray ray = camera->getCameraToViewportRay(nx, ny);

    Ogre::SceneNode* node = entity->getParentSceneNode();
    Ogre::Affine3 worldToLocal = node ? node->_getFullTransform().inverse() : Ogre::Affine3::IDENTITY;
    Ogre::Vector3 localOrigin = worldToLocal * ray.getOrigin();
    Ogre::Vector3 localDir = worldToLocal.linear() * ray.getDirection();
    localDir.normalise();

    int globalTriOffset = 0;
    for (const auto& sub : mesh->subMeshes()) {
        for (size_t ti = 0; ti < sub.triangles.size(); ++ti) {
            if (globalTriOffset + static_cast<int>(ti) != triHit) continue;
            const auto& tri = sub.triangles[ti];
            const auto& v0 = sub.vertices[tri.indices[0]];
            const auto& v1 = sub.vertices[tri.indices[1]];
            const auto& v2 = sub.vertices[tri.indices[2]];
            // Möller–Trumbore for explicit barycentric coords.
            const Ogre::Vector3 e1 = v1.position - v0.position;
            const Ogre::Vector3 e2 = v2.position - v0.position;
            const Ogre::Vector3 pvec = localDir.crossProduct(e2);
            const Ogre::Real det = e1.dotProduct(pvec);
            if (std::abs(det) < 1e-8f) return false;
            const Ogre::Real invDet = 1.0f / det;
            const Ogre::Vector3 tvec = localOrigin - v0.position;
            const Ogre::Real u = tvec.dotProduct(pvec) * invDet;
            if (u < 0.0f || u > 1.0f) return false;
            const Ogre::Vector3 qvec = tvec.crossProduct(e1);
            const Ogre::Real v = localDir.dotProduct(qvec) * invDet;
            if (v < 0.0f || u + v > 1.0f) return false;
            // u,v are barycentric coords for v1,v2; w for v0.
            const Ogre::Real w = 1.0f - u - v;
            if (!v0.hasUV || !v1.hasUV || !v2.hasUV) return false;
            outUV = v0.uv * w + v1.uv * u + v2.uv * v;
            return true;
        }
        globalTriOffset += static_cast<int>(sub.triangles.size());
    }
    return false;
}

bool TexturePaintController::beginStroke(OgreWidget* widget, const QPoint& screenPos)
{
    if (!m_paintEnabled || m_strokeActive) return false;
    if (!hasActiveSession()) {
        if (!ensurePaintableTexture(m_buffer.width() > 0 ? m_buffer.width() : 1024))
            return false;
    }
    m_strokeActive = true;
    m_strokePreSnapshot = snapshotPixels();
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Texture paint stroke begin (radius=%1 strength=%2 color=%3)")
            .arg(m_radiusUV, 0, 'f', 3)
            .arg(m_strength, 0, 'f', 3)
            .arg(m_color.name(QColor::HexRgb)));
    updateStroke(widget, screenPos);
    return true;
}

void TexturePaintController::updateStroke(OgreWidget* widget, const QPoint& screenPos)
{
    if (!m_strokeActive || !m_paintEnabled) return;
    Ogre::Vector2 uv;
    if (!hitTestUV(screenPos, widget, uv)) return;
    const Ogre::ColourValue paint(
        static_cast<float>(m_color.redF()),
        static_cast<float>(m_color.greenF()),
        static_cast<float>(m_color.blueF()),
        static_cast<float>(m_color.alphaF()));
    const int painted = m_buffer.paintBrush(uv,
                                            static_cast<float>(m_radiusUV),
                                            paint,
                                            static_cast<float>(m_strength),
                                            static_cast<float>(m_falloff));
    if (painted > 0)
        flushDirtyToOgre();
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

int TexturePaintController::bakeVertexColorsToTexture(int resolution,
                                                      int dilation,
                                                      const QString& savePath)
{
    auto* edit = EditModeController::instance();
    if (!edit || !edit->isEditModeActive() || !edit->currentMesh())
        return -1;
    const int res = resolution > 0 ? resolution
                                   : (m_buffer.width() > 0 ? m_buffer.width() : 1024);
    VertexColorBaker::Options opts;
    opts.resolution = res;
    opts.dilationPixels = std::max(0, dilation);
    const int painted = VertexColorBaker::bake(*edit->currentMesh(), m_buffer, opts);

    // Push the baked buffer to a fresh Ogre texture so the viewport sees it.
    auto* entity = edit->editEntity();
    if (entity) {
        m_sessionEntity = entity;
        m_ogreTexture.reset();
        static unsigned int s_bakeUnique = 0;
        QString hint = QStringLiteral("QMEBake_%1_%2")
                           .arg(QString::fromStdString(entity->getName()))
                           .arg(++s_bakeUnique);
        createOgreTextureFromBuffer(entity, hint);
    }
    if (!savePath.isEmpty())
        m_buffer.save(savePath.toStdString());

    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Vertex→Texture bake: %1×%1 (%2 pixels, dilation=%3)")
            .arg(res).arg(painted).arg(opts.dilationPixels));

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
    if (m_ogreTexture) {
        try {
            Ogre::TextureManager::getSingleton().remove(m_ogreTexture);
        } catch (...) {}
        m_ogreTexture.reset();
    }
    m_buffer = TexturePaintBuffer();
    m_textureName.clear();
    m_sessionEntity = nullptr;
    m_strokePreSnapshot.clear();
    emit sessionChanged();
}
