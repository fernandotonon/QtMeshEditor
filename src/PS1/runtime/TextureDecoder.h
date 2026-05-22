#ifndef TEXTUREDECODER_H
#define TEXTUREDECODER_H

#include "CaptureTypes.h"

#include <QHash>
#include <QImage>
#include <QRect>
#include <QString>
#include <QStringList>

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

    /** Dedupe key: TPAGE + CLUT + bit depth + GP0 semi-transparency mode. */
    struct MaterialKey {
        uint16_t tpage = 0;
        uint16_t clutX = 0;
        uint16_t clutY = 0;
        BitDepth bitDepth = BitDepth::Bpp15;
        uint8_t semiTrans = 0;
        uint32_t drawModeBits = 0;

        bool operator==(const MaterialKey &other) const;
    };

    struct DecodeStats {
        int cacheHits = 0;
        int cacheMisses = 0;
        int decodedMaterials = 0;
        qint64 rgbaBytes = 0;
    };

    static BitDepth bitDepthFromTpage(uint16_t tpage);
    static void clutCoordsFromClutWord(uint16_t clut, uint16_t &clutXOut, uint16_t &clutYOut);
    static MaterialKey materialKeyFromPrim(const PrimRecord &prim);
    static bool isTexturedPrim(const PrimRecord &prim);
    static void accumulateUvBounds(const PrimRecord &prim, QRect &boundsOnPage);

    /** Decode texels in @p uvBoundsOnPage (page-local 0..256). Results are cached per MaterialKey. */
    QImage decodeMaterial(const VramSnapshot &vram, const MaterialKey &key, const QRect &uvBoundsOnPage,
                          QString *errorOut = nullptr);

    QImage cachedMaterial(const MaterialKey &key) const;
    QRect cachedBoundsOnPage(const MaterialKey &key) const;
    DecodeStats stats() const { return m_stats; }
    const QStringList &warnings() const { return m_warnings; }
    void clearCache();
    void clearWarnings();

    /** Back-compat alias used by older call sites. */
    using TileKey = MaterialKey;
    QImage decodeTile(const VramSnapshot &vram, const MaterialKey &key, const QRect &regionOnPage,
                      QString *errorOut = nullptr)
    {
        return decodeMaterial(vram, key, regionOnPage, errorOut);
    }
    QImage cachedTile(const MaterialKey &key) const { return cachedMaterial(key); }

private:
    struct CachedMaterial {
        QImage image;
        QRect boundsOnPage;
    };

    void warnOutOfVram(const QString &message);
    uint16_t readVramPixel(const VramSnapshot &vram, int x, int y);
    uint16_t readClutColor(const VramSnapshot &vram, int clutX, int clutY, int index,
                           BitDepth bitDepth, const MaterialKey &key);
    QImage decodeRegion(const VramSnapshot &vram, const MaterialKey &key, const QRect &region);

    QHash<MaterialKey, CachedMaterial> m_cache;
    DecodeStats m_stats;
    QStringList m_warnings;
};

size_t qHash(const TextureDecoder::MaterialKey &key, size_t seed = 0);

#endif // TEXTUREDECODER_H
