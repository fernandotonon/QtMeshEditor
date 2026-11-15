#pragma once

#include <QImage>
#include <QString>

#include <cstddef>

// WebP image decoding (libwebp, decoder only).
//
// Neither Ogre's stb_image codec nor our Qt build (no WebP image plugin) can
// read WebP, yet glTF assets exported by trimesh — TRELLIS.2's own downloads,
// many web pipelines — embed their textures as WebP (EXT_texture_webp).
// MaterialProcessor sends such bytes here instead of to Ogre.
//
// Pure data: no Ogre, unit-tested in WebPImage_test.cpp.
namespace WebPImage {

// True when the bytes start with a WebP container header ("RIFF" ....
// "WEBP"). Detection is by content, not by file extension or MIME hint,
// because embedded glTF images often carry neither.
bool looksLikeWebP(const void* data, std::size_t size);

// Decodes a WebP image (lossy, lossless, with or without alpha) to an RGBA
// QImage (QImage::Format_RGBA8888 — bytes R,G,B,A, i.e. Ogre PF_BYTE_RGBA).
// Returns a null QImage and sets *error when the data is not a decodable
// WebP image.
QImage decode(const void* data, std::size_t size, QString* error = nullptr);

} // namespace WebPImage
