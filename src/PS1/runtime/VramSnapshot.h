#ifndef VRAMSNAPSHOT_H
#define VRAMSNAPSHOT_H

#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

#include <cstdint>

/**
 * Full PS1 VRAM buffer (1024×512×16-bit cells) for dump + texture decode (#420).
 */
class VramSnapshot
{
public:
    static constexpr int kWidth = 1024;
    static constexpr int kHeight = 512;

    enum class ViewMode {
        Native16,
        As4bpp,
        As8bpp,
        ClutPreview,
    };

    VramSnapshot();

    void clear(uint16_t fill = 0);
    bool isEmpty() const;
    /** True when at least @p minNonZero cells are non-zero (VRAM may be mostly black in-game). */
    bool hasVisibleContent(int minNonZero = 64) const;

    void writeRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels);
    uint16_t pixel(int x, int y) const;
    void setPixel(int x, int y, uint16_t value);

    const uint16_t *data() const { return m_pixels.constData(); }
    QVector<uint16_t> &mutablePixels() { return m_pixels; }

    QImage toImage(ViewMode mode, int clutX = 0, int clutY = 0) const;
    bool savePng(const QString &path) const;

    /** TPAGE top-left in VRAM texel coordinates (256×256 page). */
    static QRect tpageRect(uint16_t tpage);

private:
    QVector<uint16_t> m_pixels;
};

#endif // VRAMSNAPSHOT_H
