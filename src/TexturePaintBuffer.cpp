#include "TexturePaintBuffer.h"

#include <QImage>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

inline uint8_t floatToByte(float v)
{
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return static_cast<uint8_t>(std::lround(v * 255.0f));
}

inline float byteToFloat(uint8_t v)
{
    return static_cast<float>(v) / 255.0f;
}

} // namespace

TexturePaintBuffer::TexturePaintBuffer(int width, int height)
{
    resize(width, height);
}

void TexturePaintBuffer::resize(int width, int height)
{
    m_width = std::max(0, width);
    m_height = std::max(0, height);
    const size_t n = static_cast<size_t>(m_width) * static_cast<size_t>(m_height) * 4u;
    m_pixels.assign(n, 0xFF);
    m_dirty = {};
}

void TexturePaintBuffer::clear(const Ogre::ColourValue& color)
{
    if (m_width <= 0 || m_height <= 0) return;
    const uint8_t r = floatToByte(color.r);
    const uint8_t g = floatToByte(color.g);
    const uint8_t b = floatToByte(color.b);
    const uint8_t a = floatToByte(color.a);
    for (size_t i = 0; i < m_pixels.size(); i += 4) {
        m_pixels[i + 0] = r;
        m_pixels[i + 1] = g;
        m_pixels[i + 2] = b;
        m_pixels[i + 3] = a;
    }
    expandDirty(0, 0, m_width, m_height);
}

Ogre::ColourValue TexturePaintBuffer::pixel(int x, int y) const
{
    if (x < 0 || y < 0 || x >= m_width || y >= m_height)
        return Ogre::ColourValue(0.0f, 0.0f, 0.0f, 0.0f);
    const size_t off = (static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x)) * 4u;
    return Ogre::ColourValue(
        byteToFloat(m_pixels[off + 0]),
        byteToFloat(m_pixels[off + 1]),
        byteToFloat(m_pixels[off + 2]),
        byteToFloat(m_pixels[off + 3]));
}

void TexturePaintBuffer::setPixel(int x, int y, const Ogre::ColourValue& color)
{
    if (x < 0 || y < 0 || x >= m_width || y >= m_height)
        return;
    const size_t off = (static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x)) * 4u;
    m_pixels[off + 0] = floatToByte(color.r);
    m_pixels[off + 1] = floatToByte(color.g);
    m_pixels[off + 2] = floatToByte(color.b);
    m_pixels[off + 3] = floatToByte(color.a);
    expandDirty(x, y, x + 1, y + 1);
}

void TexturePaintBuffer::uvToPixel(const Ogre::Vector2& uv, int& outX, int& outY) const
{
    // UV origin = top-left (Ogre + Qt convention). U → X, V → Y, both direct.
    // Clamp to [0, size-1] so uv = (1.0, 1.0) maps to the last in-bounds
    // texel rather than (width, height), which is out of range. Without
    // this, tools that round-trip via uvToPixel (fill seed, picker,
    // smudge) silently miss the right/bottom edge.
    outX = std::clamp(static_cast<int>(std::floor(uv.x * static_cast<float>(m_width))),
                      0, m_width - 1);
    outY = std::clamp(static_cast<int>(std::floor(uv.y * static_cast<float>(m_height))),
                      0, m_height - 1);
}

Ogre::Vector2 TexturePaintBuffer::pixelToUV(int x, int y) const
{
    if (m_width <= 0 || m_height <= 0)
        return Ogre::Vector2::ZERO;
    const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(m_width);
    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(m_height);
    return Ogre::Vector2(u, v);
}

int TexturePaintBuffer::paintBrush(const Ogre::Vector2& uv,
                                   float radiusUV,
                                   const Ogre::ColourValue& color,
                                   float strength,
                                   float falloff,
                                   BrushShape shape)
{
    return paintBrush(uv, radiusUV,
                      [color](float, float) { return color; },
                      strength, falloff, shape);
}

int TexturePaintBuffer::paintBrush(const Ogre::Vector2& uv,
                                   float radiusUV,
                                   const ColorAtFn& colorAt,
                                   float strength,
                                   float falloff,
                                   BrushShape shape)
{
    if (m_width <= 0 || m_height <= 0) return 0;
    if (radiusUV <= 0.0f) return 0;
    if (!colorAt) return 0;
    strength = std::clamp(strength, 0.0f, 1.0f);
    falloff = std::clamp(falloff, 0.0f, 1.0f);
    if (strength <= 0.0f) return 0;

    const float radiusXf = radiusUV * static_cast<float>(m_width);
    const float radiusYf = radiusUV * static_cast<float>(m_height);
    const float centerXf = uv.x * static_cast<float>(m_width);
    const float centerYf = uv.y * static_cast<float>(m_height);

    int x0 = static_cast<int>(std::floor(centerXf - radiusXf));
    int x1 = static_cast<int>(std::ceil(centerXf + radiusXf));
    int y0 = static_cast<int>(std::floor(centerYf - radiusYf));
    int y1 = static_cast<int>(std::ceil(centerYf + radiusYf));
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(m_width, x1);
    y1 = std::min(m_height, y1);
    if (x0 >= x1 || y0 >= y1) return 0;

    const float p = 1.0f + falloff * 3.0f;
    const float invRx = 1.0f / std::max(radiusXf, 1e-6f);
    const float invRy = 1.0f / std::max(radiusYf, 1e-6f);
    int affected = 0;
    int touchedX0 = x1;
    int touchedX1 = x0;
    int touchedY0 = y1;
    int touchedY1 = y0;

    const bool square = (shape == BrushShape::Square);
    for (int y = y0; y < y1; ++y) {
        const float dy = (static_cast<float>(y) + 0.5f - centerYf) * invRy;
        for (int x = x0; x < x1; ++x) {
            const float dx = (static_cast<float>(x) + 0.5f - centerXf) * invRx;
            float blend;
            if (square) {
                // Constant strength inside the AABB — no falloff, no
                // circular cull. Half-side equals radius.
                if (std::fabs(dx) > 1.0f || std::fabs(dy) > 1.0f) continue;
                blend = strength;
            } else {
                const float r2 = dx * dx + dy * dy;
                if (r2 >= 1.0f) continue;
                const float w = std::pow(1.0f - r2, p);
                blend = strength * w;
            }
            if (blend <= 0.0f) continue;
            const Ogre::ColourValue color = colorAt(dx, dy);
            const size_t off = (static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x)) * 4u;
            const float prevR = byteToFloat(m_pixels[off + 0]);
            const float prevG = byteToFloat(m_pixels[off + 1]);
            const float prevB = byteToFloat(m_pixels[off + 2]);
            const float prevA = byteToFloat(m_pixels[off + 3]);
            m_pixels[off + 0] = floatToByte(prevR + (color.r - prevR) * blend);
            m_pixels[off + 1] = floatToByte(prevG + (color.g - prevG) * blend);
            m_pixels[off + 2] = floatToByte(prevB + (color.b - prevB) * blend);
            m_pixels[off + 3] = floatToByte(prevA + (color.a - prevA) * blend);
            ++affected;
            touchedX0 = std::min(touchedX0, x);
            touchedY0 = std::min(touchedY0, y);
            touchedX1 = std::max(touchedX1, x + 1);
            touchedY1 = std::max(touchedY1, y + 1);
        }
    }

    if (affected > 0)
        expandDirty(touchedX0, touchedY0, touchedX1, touchedY1);
    return affected;
}

int TexturePaintBuffer::floodFill(int sx, int sy, const Ogre::ColourValue& fill)
{
    if (m_width <= 0 || m_height <= 0) return 0;
    if (sx < 0 || sy < 0 || sx >= m_width || sy >= m_height) return 0;
    const Ogre::ColourValue seed = pixel(sx, sy);
    const float eps = 4.0f / 255.0f;
    auto sameColor = [&seed, eps](const Ogre::ColourValue& other) {
        return std::abs(other.r - seed.r) <= eps
            && std::abs(other.g - seed.g) <= eps
            && std::abs(other.b - seed.b) <= eps
            && std::abs(other.a - seed.a) <= eps;
    };
    if (sameColor(fill)) return 0;

    std::vector<std::pair<int,int>> stack;
    stack.push_back({sx, sy});
    std::vector<uint8_t> visited(static_cast<size_t>(m_width) * m_height, 0);
    int affected = 0;
    int tx0 = m_width, tx1 = 0, ty0 = m_height, ty1 = 0;
    while (!stack.empty()) {
        auto [x, y] = stack.back();
        stack.pop_back();
        if (x < 0 || y < 0 || x >= m_width || y >= m_height) continue;
        const size_t idx = static_cast<size_t>(y) * m_width + x;
        if (visited[idx]) continue;
        if (!sameColor(pixel(x, y))) continue;
        visited[idx] = 1;
        setPixel(x, y, fill);
        ++affected;
        tx0 = std::min(tx0, x); ty0 = std::min(ty0, y);
        tx1 = std::max(tx1, x + 1); ty1 = std::max(ty1, y + 1);
        stack.push_back({x + 1, y});
        stack.push_back({x - 1, y});
        stack.push_back({x, y + 1});
        stack.push_back({x, y - 1});
    }
    return affected;
}

bool TexturePaintBuffer::save(const std::string& path) const
{
    if (m_width <= 0 || m_height <= 0) return false;
    QImage img(m_pixels.data(), m_width, m_height, m_width * 4, QImage::Format_RGBA8888);
    if (img.isNull()) return false;
    return img.save(QString::fromStdString(path));
}

bool TexturePaintBuffer::load(const std::string& path)
{
    QImage img(QString::fromStdString(path));
    if (img.isNull()) return false;
    img = img.convertToFormat(QImage::Format_RGBA8888);
    m_width = img.width();
    m_height = img.height();
    const size_t n = static_cast<size_t>(m_width) * static_cast<size_t>(m_height) * 4u;
    m_pixels.resize(n);
    for (int y = 0; y < m_height; ++y) {
        const uchar* src = img.constScanLine(y);
        uint8_t* dst = m_pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(m_width) * 4u;
        std::memcpy(dst, src, static_cast<size_t>(m_width) * 4u);
    }
    expandDirty(0, 0, m_width, m_height);
    return true;
}

void TexturePaintBuffer::expandDirty(int x0, int y0, int x1, int y1)
{
    if (x0 >= x1 || y0 >= y1) return;
    if (m_dirty.empty()) {
        m_dirty = {x0, y0, x1, y1};
        return;
    }
    m_dirty.x0 = std::min(m_dirty.x0, x0);
    m_dirty.y0 = std::min(m_dirty.y0, y0);
    m_dirty.x1 = std::max(m_dirty.x1, x1);
    m_dirty.y1 = std::max(m_dirty.y1, y1);
}
