#ifndef TEXTUREPAINTCONTROLLER_H
#define TEXTUREPAINTCONTROLLER_H

#include "TexturePaintBuffer.h"

#include <QColor>
#include <QObject>
#include <QPoint>
#include <QQmlEngine>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <OgreTexture.h>
#include <OgreVector.h>

#include <memory>
#include <vector>

class OgreWidget;

namespace Ogre {
class Entity;
class Pass;
class TextureUnitState;
} // namespace Ogre

/**
 * @brief QML_SINGLETON managing texture painting on the active edit-mode entity.
 *
 * Mirrors the EditModeController vertex-paint flow: enable a mode, hook
 * into the existing TransformOperator mouse pipeline, hit-test a ray
 * against the EditableMesh, interpolate UV from the hit triangle, and
 * paint into a CPU-side TexturePaintBuffer. Each stroke uploads only
 * the dirty rect to a live Ogre::Texture so the viewport sees changes
 * with no full re-upload.
 *
 * Save / load round-trips the buffer to PNG (or any QImage-supported
 * format). Bake builds the same buffer from EditableMesh vertex colors
 * and is exposed as a Q_INVOKABLE so the QML "Bake" button hits the
 * same code path as the CLI.
 *
 * The controller does **not** own the Ogre::Texture once it has been
 * created — Ogre's TextureManager retains ownership, and the buffer
 * is a CPU-side mirror. Closing edit mode flushes the buffer to the
 * texture one last time and resets the session.
 */
class TexturePaintController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool texturePaintEnabled READ texturePaintEnabled WRITE setTexturePaintEnabled NOTIFY texturePaintChanged)
    Q_PROPERTY(QColor texturePaintColor READ texturePaintColor WRITE setTexturePaintColor NOTIFY texturePaintChanged)
    Q_PROPERTY(double texturePaintRadius READ texturePaintRadius WRITE setTexturePaintRadius NOTIFY texturePaintChanged)
    Q_PROPERTY(double texturePaintStrength READ texturePaintStrength WRITE setTexturePaintStrength NOTIFY texturePaintChanged)
    Q_PROPERTY(double texturePaintFalloff READ texturePaintFalloff WRITE setTexturePaintFalloff NOTIFY texturePaintChanged)
    Q_PROPERTY(int textureResolution READ textureResolution NOTIFY sessionChanged)
    Q_PROPERTY(QString currentTextureName READ currentTextureName NOTIFY sessionChanged)
    Q_PROPERTY(bool hasActiveSession READ hasActiveSession NOTIFY sessionChanged)

public:
    static TexturePaintController* instance();
    static TexturePaintController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    /// @name Paint-mode toggle (mirrors vertex-paint toggle on EditMode)
    /// @{
    bool texturePaintEnabled() const { return m_paintEnabled; }
    void setTexturePaintEnabled(bool enabled);
    /// @}

    /// @name Brush parameters
    /// @{
    QColor texturePaintColor() const { return m_color; }
    void setTexturePaintColor(const QColor& c);
    Q_INVOKABLE void setTexturePaintColorHex(const QString& cssColor);

    double texturePaintRadius() const { return m_radiusUV; }
    void setTexturePaintRadius(double r);

    double texturePaintStrength() const { return m_strength; }
    void setTexturePaintStrength(double s);

    double texturePaintFalloff() const { return m_falloff; }
    void setTexturePaintFalloff(double f);
    /// @}

    /// @name Session state
    /// @{
    int textureResolution() const { return m_buffer.width(); }
    QString currentTextureName() const { return m_textureName; }
    bool hasActiveSession() const { return m_buffer.width() > 0 && !m_textureName.isEmpty(); }
    /// @}

    /// @name Stroke API (called from TransformOperator mouse pipeline)
    /// @{
    bool beginStroke(OgreWidget* widget, const QPoint& screenPos);
    void updateStroke(OgreWidget* widget, const QPoint& screenPos);
    void endStroke();
    /// @}

    /**
     * @brief Create or attach a paintable texture for the active edit entity.
     *
     * If the entity's first submesh material has an existing diffuse
     * texture, that texture is loaded into the paint buffer. Otherwise
     * a blank `resolution`x`resolution` opaque-white buffer is created
     * and bound under a generated texture name (`QMEPaint_<unique>`).
     *
     * @return true if a paint session is now active.
     */
    Q_INVOKABLE bool ensurePaintableTexture(int resolution = 1024);

    /**
     * @brief Save the current paint buffer to disk.
     *
     * @param path  Absolute path. Extension determines format (PNG by default).
     * @return true on success.
     */
    Q_INVOKABLE bool savePaintBuffer(const QString& path) const;

    /**
     * @brief Load an image into the paint buffer (replaces contents) and
     * binds it as the active texture on the entity's first submesh.
     */
    Q_INVOKABLE bool loadPaintBuffer(const QString& path);

    /**
     * @brief Bake the active EditableMesh's vertex colors into the texture buffer.
     *
     * @param resolution  Square output resolution (defaults to current
     *                    buffer size if non-zero, else 1024).
     * @param dilation    Edge dilation in pixels after rasterization.
     * @param savePath    Optional path — if non-empty, the result is also
     *                    written to disk.
     * @return Pixel count rasterized (before dilation), or -1 on failure.
     */
    Q_INVOKABLE int bakeVertexColorsToTexture(int resolution = 0,
                                              int dilation = 4,
                                              const QString& savePath = QString());

    /// @brief End the session — release the paint buffer.
    Q_INVOKABLE void closeSession();

    /// Read-only access for tests.
    const TexturePaintBuffer& buffer() const { return m_buffer; }
    TexturePaintBuffer& mutableBuffer() { return m_buffer; }

    /// Internal: replace the buffer pixel data and re-upload to the
    /// live Ogre texture. Used by the undo command. Width/height must
    /// match the current buffer.
    void applyPixelSnapshot(const std::vector<uint8_t>& pixels);

signals:
    void texturePaintChanged();
    void sessionChanged();

private:
    explicit TexturePaintController(QObject* parent = nullptr);
    ~TexturePaintController() override;

    /// Try to look up the active edit-mode entity from EditModeController.
    /// Returns nullptr if no edit session is active.
    Ogre::Entity* activeEntity() const;

    /// Find/create the diffuse texture unit on the entity's first submesh.
    Ogre::TextureUnitState* findOrCreateDiffuseTextureUnit(Ogre::Entity* entity);

    /// Hit-test screen position against the active editable mesh and recover
    /// the barycentric-interpolated UV at the hit point. Returns false on miss.
    bool hitTestUV(const QPoint& screenPos, OgreWidget* widget, Ogre::Vector2& outUV) const;

    /// Allocate a new manual Ogre::Texture with current buffer dimensions
    /// and bind it onto the entity's first submesh material.
    bool createOgreTextureFromBuffer(Ogre::Entity* entity, const QString& nameHint);

    /// Upload buffer.dirtyRect() into the live Ogre texture and clear it.
    void flushDirtyToOgre();

    /// One-time deep-copy snapshot of pixel buffer for undo.
    std::vector<uint8_t> snapshotPixels() const;

    bool m_paintEnabled = false;
    QColor m_color = QColor(255, 0, 0, 255);
    double m_radiusUV = 0.05;     // 5% of UV space
    double m_strength = 0.75;
    double m_falloff = 0.5;

    TexturePaintBuffer m_buffer;
    QString m_textureName;
    Ogre::TexturePtr m_ogreTexture;
    Ogre::Entity* m_sessionEntity = nullptr;

    bool m_strokeActive = false;
    std::vector<uint8_t> m_strokePreSnapshot; // for undo
    static TexturePaintController* s_instance;
};

#endif // TEXTUREPAINTCONTROLLER_H
