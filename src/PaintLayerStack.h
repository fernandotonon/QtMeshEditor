#ifndef PAINTLAYERSTACK_H
#define PAINTLAYERSTACK_H

#include "PaintLayerBlend.h"
#include "PaintSelectionMask.h"
#include "TexturePaintBuffer.h"

#include <QString>
#include <vector>

/**
 * @brief Ordered paint layers for one texture paint session (#546).
 *
 * Pure data core — no Qt/Ogre beyond QString for names. Owned by
 * TexturePaintController (the paint QML singleton).
 */
class PaintLayerStack
{
public:
    enum class LayerType {
        Paint = 0,
        Fill,
        Gradient,
        Generated,
    };

    struct Layer {
        QString name;
        LayerType type = LayerType::Paint;
        PaintLayerBlend::Mode blendMode = PaintLayerBlend::Mode::Normal;
        float opacity = 1.f;
        bool visible = true;
        bool locked = false;
        TexturePaintBuffer buffer;
        /// Per-layer mask: 255 = fully visible, 0 = fully hidden.
        /// Empty vector means no mask (all visible).
        std::vector<uint8_t> maskAlpha;
    };

    /// Full stack snapshot for undo of structural ops.
    struct Snapshot {
        std::vector<Layer> layers;
        int activeIndex = 0;
        int soloIndex = -1;
    };

    int width() const;
    int height() const;
    bool empty() const { return m_layers.empty(); }
    int layerCount() const { return static_cast<int>(m_layers.size()); }
    int activeIndex() const { return m_activeIndex; }
    int soloIndex() const { return m_soloIndex; }

    const Layer& layer(int index) const;
    Layer& layer(int index);
    Layer& activeLayer();
    const Layer& activeLayer() const;

    /// Create a stack from a flat buffer (migration / session init).
    /// Empty @p layerName defaults to "Layer 0".
    void initFromFlatBuffer(const TexturePaintBuffer& flat, const QString& layerName = QString());

    void resizeAll(int width, int height);

    /// Returns index of the new layer.
    int addEmpty(const QString& name, LayerType type = LayerType::Paint);
    int addFromBuffer(const TexturePaintBuffer& src, const QString& name,
                      LayerType type = LayerType::Paint);
    int duplicateLayer(int index);
    void removeLayer(int index);
    void moveLayer(int from, int to);
    void renameLayer(int index, const QString& name);
    void mergeDown(int index);
    void flattenAll();

    void setActiveIndex(int index);
    void setVisible(int index, bool visible);
    void setLocked(int index, bool locked);
    void setOpacity(int index, float opacity);
    void setBlendMode(int index, PaintLayerBlend::Mode mode);
    void setSolo(int index, bool solo);
    void clearSolo();

    /// Composite visible layers into `out` RGBA8 buffer.
    void compositeTo(std::vector<uint8_t>& out) const;
    void compositeTo(TexturePaintBuffer& out) const;
    /// Recomposite only a pixel region into an existing RGBA8 buffer.
    void compositeRegionTo(uint8_t* outRgba, int x0, int y0, int x1, int y1) const;

    /// Union of every layer buffer's dirty rect (empty when none pending).
    TexturePaintBuffer::DirtyRect layerDirtyUnion() const;

    Snapshot snapshot() const;
    void restore(const Snapshot& snap);

    /// Ensure layer mask is sized; returns mutable mask bytes.
    std::vector<uint8_t>& ensureLayerMask(int index);
    bool hasLayerMask(int index) const;

private:
    void clampActiveIndex();
    static QString numberedLayerName(int index);
    int nextLayerNumber() const;
    QString allocateLayerName() const;
    static QString uniqueLayerName(const std::vector<Layer>& layers, const QString& base);

    std::vector<Layer> m_layers;
    int m_activeIndex = 0;
    int m_soloIndex = -1; ///< -1 = no solo
};

#endif // PAINTLAYERSTACK_H
