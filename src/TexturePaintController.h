#ifndef TEXTUREPAINTCONTROLLER_H
#define TEXTUREPAINTCONTROLLER_H

#include "TexturePaintBuffer.h"

#include <QColor>
#include <QObject>
#include <QPoint>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <OgreTexture.h>
#include <OgreVector.h>

#include <memory>
#include <vector>

class EditableMesh;
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
    // Brush params are mirrored from EditModeController so the toolbar brush
    // popup is the single source of truth — read-only here, used by QML for
    // live preview text in the Texture Paint section.
    Q_PROPERTY(QColor texturePaintColor READ texturePaintColor NOTIFY texturePaintChanged)
    Q_PROPERTY(double texturePaintRadius READ texturePaintRadius NOTIFY texturePaintChanged)
    Q_PROPERTY(double texturePaintStrength READ texturePaintStrength NOTIFY texturePaintChanged)
    Q_PROPERTY(double texturePaintFalloff READ texturePaintFalloff NOTIFY texturePaintChanged)
    Q_PROPERTY(int textureResolution READ textureResolution NOTIFY sessionChanged)
    Q_PROPERTY(QString currentTextureName READ currentTextureName NOTIFY sessionChanged)
    Q_PROPERTY(bool hasActiveSession READ hasActiveSession NOTIFY sessionChanged)

    // Brush tool — paint / erase / fill / picker.
    Q_PROPERTY(int brushTool READ brushTool WRITE setBrushTool NOTIFY brushToolChanged)

    // Live preview of the current paint buffer as a data URI, so the QML
    // preview panel can render it via Image { source: ... }. Emitted on
    // every dirty-rect flush so the preview stays in sync with strokes.
    Q_PROPERTY(QString previewDataUri READ previewDataUri NOTIFY previewChanged)

    // Texture slots on the currently-selected entity. Each entry is a
    // map: { label, submesh, slot, textureName }. QML reads this to
    // populate the slot picker.
    Q_PROPERTY(QVariantList textureSlots READ textureSlots NOTIFY slotsChanged)
    Q_PROPERTY(int activeSlotIndex READ activeSlotIndex WRITE setActiveSlotIndex NOTIFY slotsChanged)

public:
    enum BrushTool {
        ToolPaint       = 0,  ///< Lerp pixels toward brush color.
        ToolErase       = 1,  ///< Paint transparent (alpha 0). Reveals layer below if any.
        ToolFill        = 2,  ///< Flood-fill connected pixels under cursor.
        ToolColorPicker = 3,  ///< Sample color at hit UV into the brush color.
        ToolSmudge      = 4,  ///< Drag pixels in the brush direction.
    };
    Q_ENUM(BrushTool)

    static TexturePaintController* instance();
    static TexturePaintController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    /// @name Paint-mode toggle (mirrors vertex-paint toggle on EditMode)
    /// @{
    bool texturePaintEnabled() const { return m_paintEnabled; }
    void setTexturePaintEnabled(bool enabled);
    /// @}

    /// @name Brush parameters (read-only mirror of EditModeController)
    /// @{
    QColor texturePaintColor() const;
    double texturePaintRadius() const;
    double texturePaintStrength() const;
    double texturePaintFalloff() const;
    /// @}

    /// @name Session state
    /// @{
    int textureResolution() const { return m_buffer.width(); }
    QString currentTextureName() const { return m_textureName; }
    bool hasActiveSession() const { return m_buffer.width() > 0 && !m_textureName.isEmpty(); }
    /// @}

    /// @name Brush tool
    /// @{
    int brushTool() const { return static_cast<int>(m_tool); }
    void setBrushTool(int tool);
    /// @}

    /// @name Texture slot enumeration (selection-driven)
    /// @{
    QVariantList textureSlots() const { return m_slots; }
    int activeSlotIndex() const { return m_activeSlot; }
    void setActiveSlotIndex(int index);
    /// Recompute the texture slot list from the current selection.
    Q_INVOKABLE void refreshSlots();
    /// @}

    /// Preview data URI (PNG, base64) regenerated on every dirty flush.
    QString previewDataUri() const { return m_previewUri; }

    /// PNG data URI of the UV wireframe (white triangles on transparent
    /// background) at the current texture resolution. Lets the QML
    /// preview panel overlay UV islands on the texture so the user can
    /// see where their strokes are going relative to the unwrap.
    Q_PROPERTY(QString uvOverlayDataUri READ uvOverlayDataUri NOTIFY uvOverlayChanged)
    QString uvOverlayDataUri() const { return m_uvOverlayUri; }

    Q_PROPERTY(bool uvOverlayVisible READ uvOverlayVisible WRITE setUvOverlayVisible NOTIFY uvOverlayChanged)
    bool uvOverlayVisible() const { return m_uvOverlayVisible; }
    void setUvOverlayVisible(bool on);

    /// Paint via a UV coordinate directly (driven by the texture preview
    /// panel's mouse area). The preview ↔ 3D mesh hover indicator is
    /// driven by emitting hoveredUVChanged at the same time.
    Q_INVOKABLE bool beginStrokeUV(double u, double v);
    Q_INVOKABLE void updateStrokeUV(double u, double v);
    Q_INVOKABLE void endStrokeUV();
    /// Update the "hovered UV" without painting. Drives the brush ring
    /// overlay on the 3D mesh from the 2D preview panel.
    Q_INVOKABLE void setHoveredUV(double u, double v);
    Q_INVOKABLE void clearHoveredUV();

    /// Hover update from the 3D viewport — does a hit-test, emits the
    /// hovered UV signal, and draws the brush-ring overlay at the hit
    /// point in mesh-local space.
    void updateMeshHover(OgreWidget* widget, const QPoint& screenPos);
    void clearMeshHover();

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

    /// Show a native save dialog and write the paint buffer to the chosen path.
    /// Returns the path on success or empty on cancel/failure.
    Q_INVOKABLE QString savePaintBufferInteractive();

    /**
     * @brief Load an image into the paint buffer (replaces contents) and
     * binds it as the active texture on the entity's first submesh.
     */
    Q_INVOKABLE bool loadPaintBuffer(const QString& path);

    /// Show a native open dialog and load the chosen image. Returns the
    /// path on success or empty on cancel/failure.
    Q_INVOKABLE QString loadPaintBufferInteractive();

    /// Show a native color picker for the (shared) brush color. Writes
    /// back to EditModeController on accept.
    Q_INVOKABLE void pickBrushColorInteractive();

    /// Setters that mirror through to EditModeController so the toolbar
    /// brush popup and the Material-mode Paint Brush panel both stay
    /// in sync.
    Q_INVOKABLE void setBrushRadius(double r);
    Q_INVOKABLE void setBrushStrength(double s);
    Q_INVOKABLE void setBrushFalloff(double f);
    Q_INVOKABLE void setBrushColor(const QColor& c);

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
    void brushToolChanged();
    void slotsChanged();
    void previewChanged();
    void uvOverlayChanged();
    /// Emitted when the mouse hovers over a UV-mapped triangle (from
    /// the 3D mesh or from the 2D texture preview panel). u,v in [0..1];
    /// (-1, -1) means "no hover".
    void hoveredUVChanged(double u, double v);

private:
    explicit TexturePaintController(QObject* parent = nullptr);
    ~TexturePaintController() override;

    /// Currently-selected entity (or Edit Mode's active entity if Edit
    /// Mode happens to be on). Painting no longer requires Edit Mode —
    /// we keep our own private EditableMesh built from this entity.
    Ogre::Entity* activeEntity() const;

    /// Ensure the private EditableMesh is built for the active entity.
    /// Called on session creation and whenever the selection changes.
    bool ensureEditableMesh(Ogre::Entity* entity);

    /// Find the texture unit corresponding to the currently-active slot,
    /// creating one if no diffuse-like TUS exists on the selected submesh.
    Ogre::TextureUnitState* findOrCreateActiveTextureUnit(Ogre::Entity* entity);

    /// Hit-test screen position against the private editable mesh and
    /// recover the barycentric-interpolated UV at the hit point. Returns
    /// false on miss.
    bool hitTestUV(const QPoint& screenPos, OgreWidget* widget, Ogre::Vector2& outUV) const;

    /// Allocate a new manual Ogre::Texture with current buffer
    /// dimensions. When `rebindToModel` is true, also walk the
    /// entity's materials and rebind diffuse TUSes pointing at the
    /// original texture to the paint texture. When false (the default
    /// for session create), the GPU texture is allocated but the
    /// model's render is left alone until the first stroke
    /// committedly modifies pixels.
    bool createOgreTextureFromBuffer(Ogre::Entity* entity,
                                     const QString& nameHint,
                                     bool rebindToModel = false);

    /// Rebind diffuse TUSes pointing at the original-slot texture to
    /// the paint texture. Records the original bindings in
    /// `m_boundSlots` so closeSession() can restore them. Called
    /// either eagerly (rebindToModel=true on session create) or
    /// lazily (on the first stroke flush, so toggling the brush is
    /// non-destructive).
    void rebindEntityDiffuseToPaintTexture(Ogre::Entity* entity);

    /// Upload buffer.dirtyRect() into the live Ogre texture and clear it.
    void flushDirtyToOgre();

    /// Regenerate `m_previewUri` from the buffer (PNG, base64). Emits
    /// previewChanged when the URI actually changed.
    void refreshPreviewUri();

    /// Regenerate `m_uvOverlayUri` by drawing every UV-mapped triangle
    /// outline at the current texture resolution into a transparent PNG.
    void refreshUvOverlay();

    /// One-time deep-copy snapshot of pixel buffer for undo.
    std::vector<uint8_t> snapshotPixels() const;

    /// Apply the brush stamp at a UV coord using the current tool.
    /// Returns true if any pixel changed.
    bool applyBrushAtUV(const Ogre::Vector2& uv);

    /// Walk every UV-mapped triangle and return the local-space
    /// position+normal at `uv`. Used by the 2D-panel → 3D-mesh hover
    /// indicator so the user sees a brush ring on the model when they
    /// hover the texture preview.
    bool findMeshPointForUV(const Ogre::Vector2& uv,
                            Ogre::Vector3& outLocal,
                            Ogre::Vector3& outNormal) const;

    /// Draw the hover ring on the mesh at a given local position +
    /// normal. Shared between viewport-driven and panel-driven hover.
    void drawHoverRingAt(const Ogre::Vector3& localPos,
                         const Ogre::Vector3& localNormal);

    /// Flood-fill connected pixels at UV with the brush color.
    bool floodFillAtUV(const Ogre::Vector2& uv);

    /// Sample the buffer color at a UV and set it as the brush color.
    void pickColorAtUV(const Ogre::Vector2& uv);

    bool m_paintEnabled = false;
    TexturePaintBuffer m_buffer;
    QString m_textureName;
    Ogre::TexturePtr m_ogreTexture;
    Ogre::Entity* m_sessionEntity = nullptr;

    bool m_strokeActive = false;
    bool m_strokeJustBegan = false; ///< Fill/picker tools fire only once per stroke.
    std::vector<uint8_t> m_strokePreSnapshot; // for undo
    BrushTool m_tool = ToolPaint;

    /// Track every TUS we rebound to the paint texture so closeSession()
    /// can restore the originals. We keep the *material name* (not a
    /// raw pointer) so a destroyed/reloaded material doesn't dangle.
    struct BoundSlot {
        std::string materialName;
        unsigned short techIdx = 0;
        unsigned short passIdx = 0;
        unsigned short tusIdx = 0;
        std::string originalTexture;
    };
    std::vector<BoundSlot> m_boundSlots;

    // Private EditableMesh — built from the active entity so painting
    // doesn't depend on the user-facing Edit Mode workspace.
    std::unique_ptr<EditableMesh> m_paintMesh;
    Ogre::Entity* m_paintMeshEntity = nullptr;

    // Texture slot model. Populated from the selected entity's
    // materials. The active slot drives which TUS the painter writes
    // back to and which texture name the QML preview shows.
    QVariantList m_slots;
    int m_activeSlot = 0;

    // Smudge state: hold the previous stamp's pre-paint sample so the
    // next stamp can copy it forward.
    Ogre::Vector2 m_smudgePrev = Ogre::Vector2::ZERO;
    bool m_smudgeHavePrev = false;

    // Brush-ring overlay on the 3D mesh surface — drawn at the
    // current hover hit point so the user sees brush size in world
    // units while painting.
    Ogre::SceneNode* m_ringNode = nullptr;
    Ogre::ManualObject* m_ringObj = nullptr;

    // Preview PNG cache.
    QString m_previewUri;
    /// Debounce flag: prevents stroke moves from regenerating the
    /// base64 PNG on every dirty flush (expensive at 1024×1024).
    bool m_previewRefreshScheduled = false;

    // UV wireframe overlay (data: URI of a transparent PNG with the
    // triangle outlines drawn at the texture resolution).
    QString m_uvOverlayUri;
    bool m_uvOverlayVisible = false;

    static TexturePaintController* s_instance;
};

#endif // TEXTUREPAINTCONTROLLER_H
