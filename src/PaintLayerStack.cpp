#include "PaintLayerStack.h"

#include <QRegularExpression>

#include <algorithm>
#include <cstring>

namespace {

void copyBuffer(const TexturePaintBuffer& src, TexturePaintBuffer& dst)
{
    dst.resize(src.width(), src.height());
    if (!src.data().empty())
        std::memcpy(dst.data().data(), src.data().data(), src.data().size());
    dst.clearDirty();
}

} // namespace

int PaintLayerStack::width() const
{
    return m_layers.empty() ? 0 : m_layers.front().buffer.width();
}

int PaintLayerStack::height() const
{
    return m_layers.empty() ? 0 : m_layers.front().buffer.height();
}

const PaintLayerStack::Layer& PaintLayerStack::layer(int index) const
{
    return m_layers.at(static_cast<size_t>(index));
}

PaintLayerStack::Layer& PaintLayerStack::layer(int index)
{
    return m_layers.at(static_cast<size_t>(index));
}

PaintLayerStack::Layer& PaintLayerStack::activeLayer()
{
    return m_layers.at(static_cast<size_t>(m_activeIndex));
}

const PaintLayerStack::Layer& PaintLayerStack::activeLayer() const
{
    return m_layers.at(static_cast<size_t>(m_activeIndex));
}

void PaintLayerStack::initFromFlatBuffer(const TexturePaintBuffer& flat,
                                         const QString& layerName)
{
    m_layers.clear();
    m_soloIndex = -1;
    Layer bg;
    bg.name = layerName.isEmpty() ? numberedLayerName(0) : layerName;
    bg.type = LayerType::Paint;
    copyBuffer(flat, bg.buffer);
    m_layers.push_back(std::move(bg));
    m_activeIndex = 0;
}

QString PaintLayerStack::numberedLayerName(int index)
{
    return QStringLiteral("Layer %1").arg(index);
}

int PaintLayerStack::nextLayerNumber() const
{
    static const QRegularExpression re(QStringLiteral("^Layer (\\d+)$"));
    int maxNum = -1;
    for (const auto& L : m_layers) {
        const auto match = re.match(L.name);
        if (match.hasMatch())
            maxNum = std::max(maxNum, match.captured(1).toInt());
    }
    return maxNum + 1;
}

QString PaintLayerStack::allocateLayerName() const
{
    int n = nextLayerNumber();
    QString name = numberedLayerName(n);
    auto exists = [&](const QString& candidate) {
        for (const auto& L : m_layers) {
            if (L.name == candidate) return true;
        }
        return false;
    };
    while (exists(name))
        name = numberedLayerName(++n);
    return name;
}

void PaintLayerStack::resizeAll(int width, int height)
{
    const int oldW = this->width();
    const int oldH = this->height();
    for (auto& L : m_layers) {
        if (!L.maskAlpha.empty() && oldW > 0 && oldH > 0) {
            std::vector<uint8_t> oldMask = std::move(L.maskAlpha);
            L.buffer.resize(width, height);
            L.maskAlpha.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 255);
            const int copyW = std::min(oldW, width);
            const int copyH = std::min(oldH, height);
            for (int y = 0; y < copyH; ++y) {
                for (int x = 0; x < copyW; ++x) {
                    L.maskAlpha[static_cast<size_t>(y) * static_cast<size_t>(width)
                                + static_cast<size_t>(x)]
                        = oldMask[static_cast<size_t>(y) * static_cast<size_t>(oldW)
                                  + static_cast<size_t>(x)];
                }
            }
        } else {
            L.buffer.resize(width, height);
        }
    }
}

QString PaintLayerStack::uniqueLayerName(const std::vector<Layer>& layers,
                                         const QString& base)
{
    QString name = base;
    int suffix = 1;
    auto exists = [&](const QString& n) {
        for (const auto& L : layers) {
            if (L.name == n) return true;
        }
        return false;
    };
    while (exists(name))
        name = QStringLiteral("%1 %2").arg(base).arg(++suffix);
    return name;
}

int PaintLayerStack::addEmpty(const QString& name, LayerType type)
{
    const int w = width();
    const int h = height();
    Layer L;
    L.name = name.isEmpty() ? allocateLayerName() : uniqueLayerName(m_layers, name);
    L.type = type;
    if (w > 0 && h > 0) {
        L.buffer.resize(w, h);
        L.buffer.clear(Ogre::ColourValue(0.f, 0.f, 0.f, 0.f)); // transparent
        L.buffer.clearDirty();
    }
    m_layers.push_back(std::move(L));
    m_activeIndex = layerCount() - 1;
    return m_activeIndex;
}

int PaintLayerStack::addFromBuffer(const TexturePaintBuffer& src,
                                   const QString& name,
                                   LayerType type)
{
    Layer L;
    L.name = name.isEmpty() ? allocateLayerName() : uniqueLayerName(m_layers, name);
    L.type = type;
    copyBuffer(src, L.buffer);
    if (m_layers.empty()) {
        m_layers.push_back(std::move(L));
        m_activeIndex = 0;
        return 0;
    }
    const int w = width();
    const int h = height();
    if (src.width() != w || src.height() != h)
        L.buffer.resize(w, h);
    m_layers.push_back(std::move(L));
    m_activeIndex = layerCount() - 1;
    return m_activeIndex;
}

int PaintLayerStack::duplicateLayer(int index)
{
    if (index < 0 || index >= layerCount()) return m_activeIndex;
    const Layer& src = layer(index);
    Layer copy = src;
    copy.name = allocateLayerName();
    copy.locked = false;
    m_layers.insert(m_layers.begin() + index + 1, std::move(copy));
    if (m_soloIndex >= 0 && m_soloIndex > index)
        ++m_soloIndex;
    m_activeIndex = index + 1;
    return m_activeIndex;
}

void PaintLayerStack::removeLayer(int index)
{
    if (index < 0 || index >= layerCount()) return;
    if (layerCount() <= 1) return; // keep at least one layer
    m_layers.erase(m_layers.begin() + index);
    if (m_soloIndex == index) m_soloIndex = -1;
    else if (m_soloIndex > index) --m_soloIndex;
    clampActiveIndex();
}

void PaintLayerStack::moveLayer(int from, int to)
{
    if (from < 0 || from >= layerCount() || to < 0 || to >= layerCount()) return;
    if (from == to) return;
    Layer item = std::move(m_layers[static_cast<size_t>(from)]);
    m_layers.erase(m_layers.begin() + from);
    m_layers.insert(m_layers.begin() + to, std::move(item));
    if (m_activeIndex == from) m_activeIndex = to;
    else if (from < m_activeIndex && to >= m_activeIndex) --m_activeIndex;
    else if (from > m_activeIndex && to <= m_activeIndex) ++m_activeIndex;
    if (m_soloIndex == from) m_soloIndex = to;
    else if (from < m_soloIndex && to >= m_soloIndex) --m_soloIndex;
    else if (from > m_soloIndex && to <= m_soloIndex) ++m_soloIndex;
}

void PaintLayerStack::renameLayer(int index, const QString& name)
{
    if (name.isEmpty()) return;
    layer(index).name = name;
}

void PaintLayerStack::mergeDown(int index)
{
    if (index <= 0 || index >= layerCount()) return;
    Layer& upper = layer(index);
    Layer& lower = layer(index - 1);
    if (upper.buffer.width() != lower.buffer.width()
        || upper.buffer.height() != lower.buffer.height())
        return;

    std::vector<uint8_t> composite;
    std::vector<PaintLayerBlend::LayerInput> inputs(2);
    inputs[0] = {lower.buffer.data().data(), lower.maskAlpha.empty() ? nullptr : lower.maskAlpha.data(),
                 lower.blendMode, lower.opacity, true};
    inputs[1] = {upper.buffer.data().data(), upper.maskAlpha.empty() ? nullptr : upper.maskAlpha.data(),
                 upper.blendMode, upper.opacity, true};
    PaintLayerBlend::compositeLayers(lower.buffer.width(), lower.buffer.height(), inputs, composite);
    std::memcpy(lower.buffer.data().data(), composite.data(), composite.size());
    lower.buffer.markDirty(0, 0, lower.buffer.width(), lower.buffer.height());
    lower.blendMode = PaintLayerBlend::Mode::Normal;
    lower.opacity = 1.f;
    lower.maskAlpha.clear();
    removeLayer(index);
    m_activeIndex = index - 1;
}

void PaintLayerStack::flattenAll()
{
    if (layerCount() <= 1) return;
    std::vector<uint8_t> flat;
    compositeTo(flat);
    Layer merged;
    merged.name = numberedLayerName(0);
    merged.buffer.resize(width(), height());
    std::memcpy(merged.buffer.data().data(), flat.data(), flat.size());
    merged.buffer.clearDirty();
    m_layers.clear();
    m_layers.push_back(std::move(merged));
    m_activeIndex = 0;
    m_soloIndex = -1;
}

void PaintLayerStack::setActiveIndex(int index)
{
    if (index < 0 || index >= layerCount()) return;
    m_activeIndex = index;
}

void PaintLayerStack::setVisible(int index, bool visible)
{
    layer(index).visible = visible;
}

void PaintLayerStack::setLocked(int index, bool locked)
{
    layer(index).locked = locked;
}

void PaintLayerStack::setOpacity(int index, float opacity)
{
    layer(index).opacity = std::clamp(opacity, 0.f, 1.f);
}

void PaintLayerStack::setBlendMode(int index, PaintLayerBlend::Mode mode)
{
    layer(index).blendMode = mode;
}

void PaintLayerStack::setSolo(int index, bool solo)
{
    if (solo && index >= 0 && index < layerCount())
        m_soloIndex = index;
    else if (m_soloIndex == index)
        m_soloIndex = -1;
}

void PaintLayerStack::clearSolo()
{
    m_soloIndex = -1;
}

void PaintLayerStack::compositeTo(std::vector<uint8_t>& out) const
{
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0) {
        out.clear();
        return;
    }

    std::vector<PaintLayerBlend::LayerInput> inputs;
    inputs.reserve(static_cast<size_t>(layerCount()));
    for (int i = 0; i < layerCount(); ++i) {
        const Layer& L = layer(i);
        const bool show = (m_soloIndex >= 0) ? (i == m_soloIndex) : L.visible;
        inputs.push_back({
            L.buffer.data().data(),
            L.maskAlpha.empty() ? nullptr : L.maskAlpha.data(),
            L.blendMode,
            L.opacity,
            show,
        });
    }
    PaintLayerBlend::compositeLayers(w, h, inputs, out);
}

void PaintLayerStack::compositeTo(TexturePaintBuffer& out) const
{
    std::vector<uint8_t> pixels;
    compositeTo(pixels);
    if (pixels.empty()) return;
    const int w = width();
    const int h = height();
    out.resize(w, h);
    std::memcpy(out.data().data(), pixels.data(), pixels.size());
    out.markDirty(0, 0, w, h);
}

TexturePaintBuffer::DirtyRect PaintLayerStack::layerDirtyUnion() const
{
    TexturePaintBuffer::DirtyRect u;
    for (const auto& L : m_layers) {
        const auto& d = L.buffer.dirtyRect();
        if (d.empty()) continue;
        if (u.empty()) {
            u = d;
            continue;
        }
        u.x0 = std::min(u.x0, d.x0);
        u.y0 = std::min(u.y0, d.y0);
        u.x1 = std::max(u.x1, d.x1);
        u.y1 = std::max(u.y1, d.y1);
    }
    return u;
}

void PaintLayerStack::compositeRegionTo(uint8_t* outRgba, int x0, int y0, int x1, int y1) const
{
    const int w = width();
    const int h = height();
    if (!outRgba || w <= 0 || h <= 0) return;

    std::vector<PaintLayerBlend::LayerInput> inputs;
    inputs.reserve(static_cast<size_t>(layerCount()));
    for (int i = 0; i < layerCount(); ++i) {
        const Layer& L = layer(i);
        const bool show = (m_soloIndex >= 0) ? (i == m_soloIndex) : L.visible;
        inputs.push_back({
            L.buffer.data().data(),
            L.maskAlpha.empty() ? nullptr : L.maskAlpha.data(),
            L.blendMode,
            L.opacity,
            show,
        });
    }
    PaintLayerBlend::compositeLayersRegion(w, h, inputs, outRgba, x0, y0, x1, y1);
}

PaintLayerStack::Snapshot PaintLayerStack::snapshot() const
{
    Snapshot s;
    s.layers = m_layers;
    s.activeIndex = m_activeIndex;
    s.soloIndex = m_soloIndex;
    return s;
}

void PaintLayerStack::restore(const Snapshot& snap)
{
    m_layers = snap.layers;
    m_activeIndex = snap.activeIndex;
    m_soloIndex = snap.soloIndex;
    clampActiveIndex();
}

std::vector<uint8_t>& PaintLayerStack::ensureLayerMask(int index)
{
    Layer& L = layer(index);
    const size_t n = static_cast<size_t>(L.buffer.width()) * static_cast<size_t>(L.buffer.height());
    if (L.maskAlpha.size() != n)
        L.maskAlpha.assign(n, 255);
    return L.maskAlpha;
}

bool PaintLayerStack::hasLayerMask(int index) const
{
    return !layer(index).maskAlpha.empty();
}

void PaintLayerStack::clampActiveIndex()
{
    if (m_layers.empty()) {
        m_activeIndex = 0;
        return;
    }
    m_activeIndex = std::clamp(m_activeIndex, 0, layerCount() - 1);
}
