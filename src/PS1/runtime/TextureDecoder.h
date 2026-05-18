#ifndef TEXTUREDECODER_H
#define TEXTUREDECODER_H

#include <QHash>
#include <QImage>
#include <QRect>
#include <QString>

#include <cstdint>

class VramSnapshot;

/** Decodes PS1 texture pages from a VRAM snapshot (#421). */
class TextureDecoder
{
public:
    enum class BitDepth : uint8_t {
        Bpp4 = 0,
        Bpp8 = 1,
        Bpp15 = 2,
    };

    struct TileKey {
        uint16_t tpage = 0;
        uint16_t clutX = 0;
        uint16_t clutY = 0;
        BitDepth bitDepth = BitDepth::Bpp15;

        bool operator==(const TileKey &other) const;
    };

    struct DecodeStats {
        int cacheHits = 0;
        int cacheMisses = 0;
        int decodedTiles = 0;
        qint64 rgbaBytes = 0;
    };

    QImage decodeTile(const VramSnapshot &vram, const TileKey &key, const QRect &regionOnPage,
                      QString *errorOut = nullptr);
    QImage cachedTile(const TileKey &key) const;
    DecodeStats stats() const { return m_stats; }
    void clearCache();

private:
    QHash<TileKey, QImage> m_cache;
    DecodeStats m_stats;
};

size_t qHash(const TextureDecoder::TileKey &key, size_t seed = 0);

#endif // TEXTUREDECODER_H
