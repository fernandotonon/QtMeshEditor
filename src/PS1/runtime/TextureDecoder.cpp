#include "TextureDecoder.h"

#include "PsxVramColor.h"
#include "VramSnapshot.h"

#include <QRgb>
#include <QtGlobal>

#include <algorithm>

namespace {

constexpr int kPageSize = 256;

bool treatZeroAsTransparent(const TextureDecoder::MaterialKey &key)
{
    return PsxVramColor::drawModeMasksZeroAsTransparent(key.drawModeBits);
}

} // namespace

bool TextureDecoder::MaterialKey::operator==(const MaterialKey &other) const
{
    return tpage == other.tpage && clutX == other.clutX && clutY == other.clutY
           && bitDepth == other.bitDepth && semiTrans == other.semiTrans
           && drawModeBits == other.drawModeBits;
}

size_t qHash(const TextureDecoder::MaterialKey &key, size_t seed)
{
    return qHash(static_cast<uint>(key.tpage), seed) ^ qHash(static_cast<uint>(key.clutX), seed << 1)
           ^ qHash(static_cast<uint>(key.clutY), seed << 2)
           ^ qHash(static_cast<int>(key.bitDepth), seed << 3)
           ^ qHash(static_cast<uint>(key.semiTrans), seed << 4)
           ^ qHash(key.drawModeBits, seed << 5);
}

TextureDecoder::BitDepth TextureDecoder::bitDepthFromTpage(uint16_t tpage)
{
    switch ((tpage >> 7) & 3) {
    case 0:
        return BitDepth::Bpp4;
    case 1:
        return BitDepth::Bpp8;
    default:
        return BitDepth::Bpp15;
    }
}

void TextureDecoder::clutCoordsFromClutWord(uint16_t clut, uint16_t &clutXOut, uint16_t &clutYOut)
{
    clutXOut = static_cast<uint16_t>((clut & 0x3F) << 4);
    clutYOut = static_cast<uint16_t>((clut >> 6) & 0x3FF);
}

TextureDecoder::MaterialKey TextureDecoder::materialKeyFromPrim(const PrimRecord &prim)
{
    MaterialKey key{};
    key.tpage = prim.tpage;
    clutCoordsFromClutWord(prim.clut, key.clutX, key.clutY);
    key.bitDepth = bitDepthFromTpage(prim.tpage);
    key.semiTrans = prim.semiTrans;
    key.drawModeBits = prim.drawModeBits;
    return key;
}

bool TextureDecoder::isTexturedPrim(const PrimRecord &prim)
{
    return prim.kind == PrimKind::TexturedTri || prim.kind == PrimKind::TexturedQuad
           || prim.kind == PrimKind::Sprite;
}

void TextureDecoder::accumulateUvBounds(const PrimRecord &prim, QRect &boundsOnPage)
{
    if (!isTexturedPrim(prim))
        return;

    for (int v = 0; v < prim.vertexCount && v < 4; ++v) {
        const int u = static_cast<int>(static_cast<uint8_t>(prim.verts[v].u));
        const int vv = static_cast<int>(static_cast<uint8_t>(prim.verts[v].v));
        const QRect texel(u, vv, 1, 1);
        boundsOnPage = boundsOnPage.isValid() ? boundsOnPage.united(texel) : texel;
    }
}

void TextureDecoder::clearCache()
{
    m_cache.clear();
    m_stats = {};
}

void TextureDecoder::clearWarnings()
{
    m_warnings.clear();
}

void TextureDecoder::warnOutOfVram(const QString &message)
{
    if (!m_warnings.contains(message))
        m_warnings.append(message);
}

uint16_t TextureDecoder::readVramPixel(const VramSnapshot &vram, int x, int y)
{
    if (x < 0 || y < 0 || x >= VramSnapshot::kWidth || y >= VramSnapshot::kHeight) {
        warnOutOfVram(QStringLiteral("VRAM read out of range (%1,%2)").arg(x).arg(y));
        return 0;
    }
    return vram.pixel(x, y);
}

uint16_t TextureDecoder::readClutColor(const VramSnapshot &vram, int clutX, int clutY, int index,
                                       BitDepth bitDepth)
{
    const int maxIndex = bitDepth == BitDepth::Bpp4 ? 15 : 255;
    const int clamped = std::clamp(index, 0, maxIndex);
    return readVramPixel(vram, clutX + clamped, clutY);
}

QImage TextureDecoder::decodeRegion(const VramSnapshot &vram, const MaterialKey &key,
                                    const QRect &region)
{
    QImage img(region.width(), region.height(), QImage::Format_ARGB32);
    img.fill(Qt::transparent);

    const QRect page = VramSnapshot::tpageRect(key.tpage);
    const bool zeroTransparent = treatZeroAsTransparent(key);

    if (key.bitDepth == BitDepth::Bpp15) {
        for (int y = 0; y < region.height(); ++y) {
            auto *scan = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < region.width(); ++x) {
                const int vramX = page.x() + region.x() + x;
                const int vramY = page.y() + region.y() + y;
                const uint16_t c = readVramPixel(vram, vramX, vramY);
                uint8_t r, g, b, a;
                PsxVramColor::bgr555ToRgba(c, r, g, b, a, zeroTransparent);
                scan[x] = qRgba(r, g, b, a);
            }
        }
        return img;
    }

    if (key.bitDepth == BitDepth::Bpp8) {
        for (int y = 0; y < region.height(); ++y) {
            auto *scan = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < region.width(); ++x) {
                const int texX = region.x() + x;
                const int texY = region.y() + y;
                const int wordX = page.x() + (texX / 2);
                const int vramY = page.y() + texY;
                const uint16_t word = readVramPixel(vram, wordX, vramY);
                const uint8_t idx = (texX & 1) ? static_cast<uint8_t>((word >> 8) & 0xFF)
                                               : static_cast<uint8_t>(word & 0xFF);
                const uint16_t c = readClutColor(vram, key.clutX, key.clutY, idx, key.bitDepth);
                uint8_t r, g, b, a;
                PsxVramColor::bgr555ToRgba(c, r, g, b, a, zeroTransparent);
                scan[x] = qRgba(r, g, b, a);
            }
        }
        return img;
    }

    for (int y = 0; y < region.height(); ++y) {
        auto *scan = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < region.width(); ++x) {
            const int texX = region.x() + x;
            const int texY = region.y() + y;
            const int wordX = page.x() + (texX / 4);
            const int vramY = page.y() + texY;
            const uint16_t word = readVramPixel(vram, wordX, vramY);
            const int shift = (texX & 3) * 4;
            const uint8_t idx = static_cast<uint8_t>((word >> shift) & 0xF);
            const uint16_t c = readClutColor(vram, key.clutX, key.clutY, idx, key.bitDepth);
            uint8_t r, g, b, a;
            PsxVramColor::bgr555ToRgba(c, r, g, b, a, zeroTransparent);
            scan[x] = qRgba(r, g, b, a);
        }
    }
    return img;
}

QImage TextureDecoder::cachedMaterial(const MaterialKey &key) const
{
    return m_cache.value(key).image;
}

QRect TextureDecoder::cachedBoundsOnPage(const MaterialKey &key) const
{
    return m_cache.value(key).boundsOnPage;
}

QImage TextureDecoder::decodeMaterial(const VramSnapshot &vram, const MaterialKey &key,
                                      const QRect &uvBoundsOnPage, QString *errorOut)
{
    QRect region = uvBoundsOnPage.isValid() ? uvBoundsOnPage : QRect(0, 0, kPageSize, kPageSize);
    region = region.intersected(QRect(0, 0, kPageSize, kPageSize));
    if (!region.isValid() || region.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("Texture UV bounds are empty");
        return {};
    }

    if (m_cache.contains(key)) {
        const CachedMaterial &cached = m_cache.value(key);
        if (cached.boundsOnPage.contains(region)) {
            ++m_stats.cacheHits;
            const QPoint offset = region.topLeft() - cached.boundsOnPage.topLeft();
            return cached.image.copy(offset.x(), offset.y(), region.width(), region.height());
        }

        const QRect merged = cached.boundsOnPage.united(region);
        QImage mergedImg = decodeRegion(vram, key, merged);
        m_cache.insert(key, CachedMaterial{mergedImg, merged});
        ++m_stats.cacheMisses;
        ++m_stats.decodedMaterials;
        m_stats.rgbaBytes += mergedImg.sizeInBytes();

        const QPoint offset = region.topLeft() - merged.topLeft();
        return mergedImg.copy(offset.x(), offset.y(), region.width(), region.height());
    }

    ++m_stats.cacheMisses;
    QImage img = decodeRegion(vram, key, region);
    if (img.isNull()) {
        if (errorOut)
            *errorOut = QStringLiteral("Texture decode failed");
        return {};
    }

    m_cache.insert(key, CachedMaterial{img, region});
    ++m_stats.decodedMaterials;
    m_stats.rgbaBytes += img.sizeInBytes();
    return img;
}
