/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/
#include "PS1/PS1TIM.h"

#include <QFile>

#include <OgrePixelFormat.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

inline uint16_t readU16le(const uint8_t* p)
{
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

inline uint32_t readU32le(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline void writeU16le(uint8_t* p, uint16_t v)
{
    p[0] = uint8_t(v & 0xFF);
    p[1] = uint8_t((v >> 8) & 0xFF);
}

inline void writeU32le(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v & 0xFF);
    p[1] = uint8_t((v >> 8) & 0xFF);
    p[2] = uint8_t((v >> 16) & 0xFF);
    p[3] = uint8_t((v >> 24) & 0xFF);
}

static void psxBgr555ToRgba(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a)
{
    // Bits: 0..4 R, 5..9 G, 10..14 B, 15 STP (semi-transparency flag in GPU)
    const uint8_t rr = uint8_t(c & 0x1F);
    const uint8_t gg = uint8_t((c >> 5) & 0x1F);
    const uint8_t bb = uint8_t((c >> 10) & 0x1F);
    r = uint8_t((rr * 255 + 15) / 31);
    g = uint8_t((gg * 255 + 15) / 31);
    b = uint8_t((bb * 255 + 15) / 31);
    // Convention: 0 is transparent in many TIMs; otherwise opaque.
    a = (c == 0) ? 0 : 255;
}

static uint16_t rgbaToPsxBgr555(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    const uint16_t r5 = uint16_t(r >> 3);
    const uint16_t g5 = uint16_t(g >> 3);
    const uint16_t b5 = uint16_t(b >> 3);
    const uint16_t stp = (a < 128) ? 1u : 0u;
    return uint16_t((stp << 15) | (b5 << 10) | (g5 << 5) | (r5 << 0));
}

struct TimImageHeader {
    uint16_t x{};
    uint16_t y{};
    uint16_t wWords{};
    uint16_t h{};
};

static bool readTimBlockHeader(const uint8_t* data, size_t size, size_t& p, uint32_t& outLenBytes, TimImageHeader& outHdr, QString* err)
{
    if (p + 12 > size) {
        if (err) *err = "TIM truncated (block header)";
        return false;
    }
    outLenBytes = readU32le(data + p);
    outHdr.x = readU16le(data + p + 4);
    outHdr.y = readU16le(data + p + 6);
    outHdr.wWords = readU16le(data + p + 8);
    outHdr.h = readU16le(data + p + 10);
    if (outLenBytes < 12) {
        if (err) *err = "TIM invalid block length";
        return false;
    }
    if (p + outLenBytes > size) {
        if (err) *err = "TIM truncated (block payload)";
        return false;
    }
    return true;
}

} // namespace

namespace PS1TIM {

bool loadTimToOgreImage(const QString& timPath, Ogre::Image& outImage, QString* outError)
{
    QFile f(timPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (outError) *outError = "Failed to open TIM";
        return false;
    }
    const QByteArray raw = f.readAll();
    f.close();
    const uint8_t* data = reinterpret_cast<const uint8_t*>(raw.constData());
    const size_t size = static_cast<size_t>(raw.size());
    if (size < 8) {
        if (outError) *outError = "TIM too small";
        return false;
    }

    const uint32_t magic = readU32le(data);
    if (magic != 0x10u) {
        if (outError) *outError = "Not a TIM (bad magic)";
        return false;
    }
    const uint32_t flags = readU32le(data + 4);
    const uint32_t bppMode = (flags & 0x7u);
    const bool hasClut = (flags & 0x8u) != 0;

    if (!(bppMode == 0 || bppMode == 1 || bppMode == 2)) {
        if (outError) *outError = "Unsupported TIM bpp mode";
        return false;
    }
    if ((bppMode == 0 || bppMode == 1) && !hasClut) {
        if (outError) *outError = "Indexed TIM missing CLUT";
        return false;
    }

    size_t p = 8;
    std::vector<uint16_t> clut;
    uint16_t clutW = 0;
    uint16_t clutH = 0;

    if (hasClut) {
        uint32_t lenBytes = 0;
        TimImageHeader ch;
        if (!readTimBlockHeader(data, size, p, lenBytes, ch, outError))
            return false;
        const size_t clutDataBytes = size_t(lenBytes) - 12u;
        clutW = ch.wWords;
        clutH = ch.h;
        if (clutDataBytes % 2u != 0) {
            if (outError) *outError = "TIM CLUT has odd byte size";
            return false;
        }
        const size_t nColors = clutDataBytes / 2u;
        clut.resize(nColors);
        const uint8_t* cp = data + p + 12;
        for (size_t i = 0; i < nColors; ++i)
            clut[i] = readU16le(cp + i * 2);
        p += lenBytes;
    }

    // Image block
    uint32_t imgLenBytes = 0;
    TimImageHeader ih;
    if (!readTimBlockHeader(data, size, p, imgLenBytes, ih, outError))
        return false;
    const uint8_t* imgData = data + p + 12;
    const size_t imgDataBytes = size_t(imgLenBytes) - 12u;

    const int height = int(ih.h);
    int widthPx = 0;
    if (bppMode == 0)      widthPx = int(ih.wWords) * 4; // 4bpp: 4 pixels per 16-bit word
    else if (bppMode == 1) widthPx = int(ih.wWords) * 2; // 8bpp: 2 pixels per 16-bit word
    else                   widthPx = int(ih.wWords);     // 16bpp: 1 pixel per 16-bit word

    if (widthPx <= 0 || height <= 0) {
        if (outError) *outError = "TIM invalid dimensions";
        return false;
    }

    const size_t expectedWords = size_t(ih.wWords) * size_t(ih.h);
    if (imgDataBytes < expectedWords * 2u) {
        if (outError) *outError = "TIM image data truncated";
        return false;
    }

    std::vector<uint8_t> rgba(size_t(widthPx) * size_t(height) * 4u, 0);

    auto writePx = [&](int x, int y, uint16_t c16) {
        uint8_t r, g, b, a;
        psxBgr555ToRgba(c16, r, g, b, a);
        const size_t idx = (size_t(y) * size_t(widthPx) + size_t(x)) * 4u;
        rgba[idx + 0] = r;
        rgba[idx + 1] = g;
        rgba[idx + 2] = b;
        rgba[idx + 3] = a;
    };

    if (bppMode == 2) {
        // Direct 16bpp
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < widthPx; ++x) {
                const size_t wi = size_t(y) * size_t(ih.wWords) + size_t(x);
                const uint16_t c = readU16le(imgData + wi * 2);
                writePx(x, y, c);
            }
        }
    } else {
        // Indexed
        const size_t clutRowStride = size_t(clutW);
        const size_t clutRow0 = 0; // first CLUT row
        if (clut.empty() || clutW == 0 || clutH == 0) {
            if (outError) *outError = "TIM missing CLUT data";
            return false;
        }
        if (bppMode == 0 && clutW < 16) {
            if (outError) *outError = "TIM 4bpp CLUT too small";
            return false;
        }
        if (bppMode == 1 && clutW < 256) {
            // Some TIMs store multiple 16-color CLUTs for 4bpp only; 8bpp should be 256.
            if (outError) *outError = "TIM 8bpp CLUT too small";
            return false;
        }

        for (int y = 0; y < height; ++y) {
            for (int w = 0; w < int(ih.wWords); ++w) {
                const uint16_t word = readU16le(imgData + (size_t(y) * size_t(ih.wWords) + size_t(w)) * 2);
                if (bppMode == 1) {
                    // low byte then high byte
                    const uint8_t i0 = uint8_t(word & 0xFF);
                    const uint8_t i1 = uint8_t((word >> 8) & 0xFF);
                    const uint16_t c0 = clut[clutRow0 * clutRowStride + i0];
                    const uint16_t c1 = clut[clutRow0 * clutRowStride + i1];
                    const int x0 = w * 2 + 0;
                    const int x1 = w * 2 + 1;
                    if (x0 < widthPx) writePx(x0, y, c0);
                    if (x1 < widthPx) writePx(x1, y, c1);
                } else {
                    // 4bpp: 4 nibbles, low->high
                    const uint8_t i0 = uint8_t((word >> 0) & 0xF);
                    const uint8_t i1 = uint8_t((word >> 4) & 0xF);
                    const uint8_t i2 = uint8_t((word >> 8) & 0xF);
                    const uint8_t i3 = uint8_t((word >> 12) & 0xF);
                    const uint16_t c0 = clut[clutRow0 * clutRowStride + i0];
                    const uint16_t c1 = clut[clutRow0 * clutRowStride + i1];
                    const uint16_t c2 = clut[clutRow0 * clutRowStride + i2];
                    const uint16_t c3 = clut[clutRow0 * clutRowStride + i3];
                    const int x0 = w * 4 + 0;
                    const int x1 = w * 4 + 1;
                    const int x2 = w * 4 + 2;
                    const int x3 = w * 4 + 3;
                    if (x0 < widthPx) writePx(x0, y, c0);
                    if (x1 < widthPx) writePx(x1, y, c1);
                    if (x2 < widthPx) writePx(x2, y, c2);
                    if (x3 < widthPx) writePx(x3, y, c3);
                }
            }
        }
    }

    // TIM pixels are in PSX VRAM space and TMD UV bytes are authored in 256×256 "page texels".
    // To make TMD mapping work without requiring material scale/scroll, embed the decoded TIM bitmap
    // into a 256×256 canvas at its VRAM-local offset (x,y), then return that canvas.
    //
    // TIM header X is in 16-bit VRAM pixels (words). Convert to texel offset depending on bpp:
    // - 4bpp: 1 word = 4 texels
    // - 8bpp: 1 word = 2 texels
    // - 16bpp: 1 word = 1 texel
    //
    // We only keep the offset within a single 256×256 page.
    constexpr int kPageW = 256;
    constexpr int kPageH = 256;
    int xTex = 0;
    if (bppMode == 0)      xTex = int(ih.x) * 4;
    else if (bppMode == 1) xTex = int(ih.x) * 2;
    else                   xTex = int(ih.x);
    int yTex = int(ih.y);
    xTex = ((xTex % kPageW) + kPageW) % kPageW;
    yTex = ((yTex % kPageH) + kPageH) % kPageH;

    std::vector<uint8_t> canvas(size_t(kPageW) * size_t(kPageH) * 4u, 0);
    for (int y = 0; y < height; ++y) {
        const int dy = yTex + y;
        if (dy < 0 || dy >= kPageH)
            continue;
        for (int x = 0; x < widthPx; ++x) {
            const int dx = xTex + x;
            if (dx < 0 || dx >= kPageW)
                continue;
            const size_t src = (size_t(y) * size_t(widthPx) + size_t(x)) * 4u;
            const size_t dst = (size_t(dy) * size_t(kPageW) + size_t(dx)) * 4u;
            canvas[dst + 0] = rgba[src + 0];
            canvas[dst + 1] = rgba[src + 1];
            canvas[dst + 2] = rgba[src + 2];
            canvas[dst + 3] = rgba[src + 3];
        }
    }

    // Ogre::Image will take ownership of the buffer when autoDelete=true.
    auto* heap = OGRE_ALLOC_T(uint8_t, canvas.size(), Ogre::MEMCATEGORY_GENERAL);
    std::copy(canvas.begin(), canvas.end(), heap);
    outImage.loadDynamicImage(heap, (size_t)kPageW, (size_t)kPageH, 1, Ogre::PF_BYTE_RGBA, true);
    return true;
}

bool saveOgreImageToTim16(const Ogre::Image& image, const QString& timPath, QString* outError)
{
    // Convert to RGBA8 if needed (Ogre 14 Image has no convert()).
    Ogre::Image img = image;
    std::vector<uint8_t> converted;
    if (img.getFormat() != Ogre::PF_BYTE_RGBA) {
        try {
            const size_t w = img.getWidth();
            const size_t h = img.getHeight();
            const Ogre::PixelBox srcBox = img.getPixelBox();
            converted.resize(w * h * 4u);
            Ogre::PixelBox dstBox(w, h, 1, Ogre::PF_BYTE_RGBA, converted.data());
            Ogre::PixelUtil::bulkPixelConversion(srcBox, dstBox);
            // Wrap as a temporary image view (no ownership).
            img.loadDynamicImage(converted.data(), w, h, 1, Ogre::PF_BYTE_RGBA, false);
        } catch (...) {
            if (outError) *outError = "Failed to convert image to RGBA8";
            return false;
        }
    }

    constexpr int kPageW = 256;
    constexpr int kPageH = 256;

    const int w = int(img.getWidth());
    const int h = int(img.getHeight());
    if (w <= 0 || h <= 0) {
        if (outError) *outError = "Invalid image dimensions";
        return false;
    }

    // Embed into a 256x256 RGBA canvas.
    std::vector<uint8_t> canvas(size_t(kPageW) * size_t(kPageH) * 4u, 0);
    const uint8_t* src = img.getData();
    const int copyW = std::min(w, kPageW);
    const int copyH = std::min(h, kPageH);
    for (int y = 0; y < copyH; ++y) {
        for (int x = 0; x < copyW; ++x) {
            const size_t si = (size_t(y) * size_t(w) + size_t(x)) * 4u;
            const size_t di = (size_t(y) * size_t(kPageW) + size_t(x)) * 4u;
            canvas[di + 0] = src[si + 0];
            canvas[di + 1] = src[si + 1];
            canvas[di + 2] = src[si + 2];
            canvas[di + 3] = src[si + 3];
        }
    }

    // TIM 16bpp (no CLUT):
    // - header: magic 0x10, flags bppMode=2 (0x02)
    // - image block: len, x, y, wWords, h, data (BGR555)
    const uint32_t flags = 0x02u;
    const uint16_t xVram = 0;
    const uint16_t yVram = 0;
    const uint16_t wWords = kPageW; // 16bpp: 1 word per pixel
    const uint16_t hWords = kPageH;
    const uint32_t imgDataBytes = uint32_t(kPageW * kPageH * 2);
    const uint32_t blockLen = 12u + imgDataBytes;

    QByteArray out;
    out.resize(int(8 + blockLen));
    uint8_t* d = reinterpret_cast<uint8_t*>(out.data());
    writeU32le(d + 0, 0x10u);
    writeU32le(d + 4, flags);
    writeU32le(d + 8, blockLen);
    writeU16le(d + 12, xVram);
    writeU16le(d + 14, yVram);
    writeU16le(d + 16, wWords);
    writeU16le(d + 18, hWords);

    uint8_t* px = d + 20;
    for (int y = 0; y < kPageH; ++y) {
        for (int x = 0; x < kPageW; ++x) {
            const size_t ci = (size_t(y) * size_t(kPageW) + size_t(x)) * 4u;
            const uint16_t c16 = rgbaToPsxBgr555(canvas[ci + 0], canvas[ci + 1], canvas[ci + 2], canvas[ci + 3]);
            writeU16le(px + (size_t(y) * size_t(kPageW) + size_t(x)) * 2u, c16);
        }
    }

    QFile f(timPath);
    if (!f.open(QIODevice::WriteOnly)) {
        if (outError) *outError = "Failed to open output TIM for write";
        return false;
    }
    const qint64 wrote = f.write(out);
    f.close();
    if (wrote != out.size()) {
        if (outError) *outError = "Failed to write TIM";
        return false;
    }
    return true;
}

} // namespace PS1TIM

