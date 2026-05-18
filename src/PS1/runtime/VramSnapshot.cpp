#include "VramSnapshot.h"

#include "PsxVramColor.h"

#include <QRgb>

VramSnapshot::VramSnapshot()
{
    m_pixels.resize(kWidth * kHeight);
    clear();
}

void VramSnapshot::clear(uint16_t fill)
{
    m_pixels.fill(fill);
}

bool VramSnapshot::isEmpty() const
{
    for (uint16_t v : m_pixels) {
        if (v != 0)
            return false;
    }
    return true;
}

void VramSnapshot::writeRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels)
{
    if (!pixels || w == 0 || h == 0)
        return;

    for (uint16_t row = 0; row < h; ++row) {
        const int dstY = static_cast<int>(y) + row;
        if (dstY < 0 || dstY >= kHeight)
            continue;
        for (uint16_t col = 0; col < w; ++col) {
            const int dstX = static_cast<int>(x) + col;
            if (dstX < 0 || dstX >= kWidth)
                continue;
            m_pixels[dstY * kWidth + dstX] = pixels[row * w + col];
        }
    }
}

uint16_t VramSnapshot::pixel(int x, int y) const
{
    if (x < 0 || y < 0 || x >= kWidth || y >= kHeight)
        return 0;
    return m_pixels[y * kWidth + x];
}

void VramSnapshot::setPixel(int x, int y, uint16_t value)
{
    if (x < 0 || y < 0 || x >= kWidth || y >= kHeight)
        return;
    m_pixels[y * kWidth + x] = value;
}

QRect VramSnapshot::tpageRect(uint16_t tpage)
{
    const int pageX = ((tpage >> 0) & 0xF) * 64;
    const int pageY = ((tpage >> 4) & 0x1) * 256;
    return QRect(pageX, pageY, 256, 256);
}

QImage VramSnapshot::toImage(ViewMode mode, int clutX, int clutY) const
{
    QImage img(kWidth, kHeight, QImage::Format_ARGB32);
    img.fill(Qt::black);

    for (int y = 0; y < kHeight; ++y) {
        auto *scan = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < kWidth; ++x) {
            const uint16_t cell = pixel(x, y);
            if (mode == ViewMode::Native16) {
                uint8_t r, g, b, a;
                PsxVramColor::bgr555ToRgba(cell, r, g, b, a, false);
                scan[x] = qRgba(r, g, b, a);
            } else if (mode == ViewMode::As4bpp) {
                const int idx = cell & 0xF;
                scan[x] = qRgb(idx * 17, idx * 17, idx * 17);
            } else if (mode == ViewMode::As8bpp) {
                const int idx = cell & 0xFF;
                scan[x] = qRgb(idx, idx, idx);
            } else {
                const int idx = (cell & 0xF);
                const uint16_t clutColor = pixel(clutX + idx, clutY);
                uint8_t r, g, b, a;
                PsxVramColor::bgr555ToRgba(clutColor, r, g, b, a, false);
                scan[x] = qRgba(r, g, b, a);
            }
        }
    }
    return img;
}

bool VramSnapshot::savePng(const QString &path) const
{
    return toImage(ViewMode::Native16).save(path, "PNG");
}
