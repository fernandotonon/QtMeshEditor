#include "TextureDecoder.h"

#include "PsxVramColor.h"
#include "VramSnapshot.h"

#include <QRgb>

bool TextureDecoder::TileKey::operator==(const TileKey &other) const
{
    return tpage == other.tpage && clutX == other.clutX && clutY == other.clutY
           && bitDepth == other.bitDepth && regionOnPage == other.regionOnPage;
}

size_t qHash(const TextureDecoder::TileKey &key, size_t seed)
{
    return qHash(static_cast<uint>(key.tpage), seed) ^ qHash(static_cast<uint>(key.clutX), seed << 1)
           ^ qHash(static_cast<uint>(key.clutY), seed << 2)
           ^ qHash(static_cast<int>(key.bitDepth), seed << 3)
           ^ qHash(key.regionOnPage, seed << 4);
}

void TextureDecoder::clearCache()
{
    m_cache.clear();
    m_stats = {};
}

QImage TextureDecoder::cachedTile(const TileKey &key) const
{
    return m_cache.value(key);
}

QImage TextureDecoder::decodeTile(const VramSnapshot &vram, const TileKey &key,
                                  const QRect &regionOnPage, QString *errorOut)
{
    const QRect page = VramSnapshot::tpageRect(key.tpage);
    QRect region = regionOnPage.isValid() ? regionOnPage : QRect(0, 0, 256, 256);
    region = region.intersected(QRect(0, 0, 256, 256));
    if (!region.isValid() || region.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("Texture region is empty");
        return {};
    }

    TileKey cacheKey = key;
    cacheKey.regionOnPage = region;
    if (m_cache.contains(cacheKey)) {
        ++m_stats.cacheHits;
        return m_cache.value(cacheKey);
    }

    ++m_stats.cacheMisses;

    QImage img(region.width(), region.height(), QImage::Format_ARGB32);
    img.fill(Qt::transparent);

    const int clutX = static_cast<int>(key.clutX);
    const int clutY = static_cast<int>(key.clutY);

    if (key.bitDepth == BitDepth::Bpp15) {
        for (int y = 0; y < region.height(); ++y) {
            auto *scan = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < region.width(); ++x) {
                const int vramX = page.x() + region.x() + x;
                const int vramY = page.y() + region.y() + y;
                const uint16_t c = vram.pixel(vramX, vramY);
                uint8_t r, g, b, a;
                PsxVramColor::bgr555ToRgba(c, r, g, b, a, true);
                scan[x] = qRgba(r, g, b, a);
            }
        }
    } else if (key.bitDepth == BitDepth::Bpp8) {
        for (int y = 0; y < region.height(); ++y) {
            auto *scan = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < region.width(); ++x) {
                const int texX = region.x() + x;
                const int texY = region.y() + y;
                const int wordX = page.x() + (texX / 2);
                const int vramY = page.y() + texY;
                const uint16_t word = vram.pixel(wordX, vramY);
                const uint8_t idx = (texX & 1) ? static_cast<uint8_t>((word >> 8) & 0xFF)
                                                : static_cast<uint8_t>(word & 0xFF);
                const uint16_t c = vram.pixel(clutX + idx, clutY);
                uint8_t r, g, b, a;
                PsxVramColor::bgr555ToRgba(c, r, g, b, a, true);
                scan[x] = qRgba(r, g, b, a);
            }
        }
    } else {
        for (int y = 0; y < region.height(); ++y) {
            auto *scan = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < region.width(); ++x) {
                const int texX = region.x() + x;
                const int texY = region.y() + y;
                const int wordX = page.x() + (texX / 4);
                const int vramY = page.y() + texY;
                const uint16_t word = vram.pixel(wordX, vramY);
                const int shift = (texX & 3) * 4;
                const uint8_t idx = static_cast<uint8_t>((word >> shift) & 0xF);
                const uint16_t c = vram.pixel(clutX + idx, clutY);
                uint8_t r, g, b, a;
                PsxVramColor::bgr555ToRgba(c, r, g, b, a, true);
                scan[x] = qRgba(r, g, b, a);
            }
        }
    }

    m_cache.insert(cacheKey, img);
    ++m_stats.decodedTiles;
    m_stats.rgbaBytes += img.sizeInBytes();
    return img;
}
