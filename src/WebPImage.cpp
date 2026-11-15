#include "WebPImage.h"

#include <cstdint>
#include <cstring>

#include <webp/decode.h>

namespace WebPImage {

bool looksLikeWebP(const void* data, std::size_t size)
{
    if (!data || size < 12)
        return false;
    const auto* b = static_cast<const unsigned char*>(data);
    return std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WEBP", 4) == 0;
}

QImage decode(const void* data, std::size_t size, QString* error)
{
    auto fail = [error](const QString& why) {
        if (error)
            *error = why;
        return QImage();
    };
    if (!looksLikeWebP(data, size))
        return fail(QStringLiteral("not a WebP image (no RIFF/WEBP header)"));

    const auto* bytes = static_cast<const uint8_t*>(data);
    int width = 0;
    int height = 0;
    if (!WebPGetInfo(bytes, size, &width, &height) || width <= 0 || height <= 0)
        return fail(QStringLiteral("corrupt WebP header"));

    QImage image(width, height, QImage::Format_RGBA8888);
    if (image.isNull())
        return fail(QStringLiteral("out of memory for a %1x%2 WebP image").arg(width).arg(height));

    // Decode straight into the QImage's rows (its stride may be padded).
    const auto stride = static_cast<int>(image.bytesPerLine());
    const auto bufferSize = static_cast<std::size_t>(image.sizeInBytes());
    if (!WebPDecodeRGBAInto(bytes, size, image.bits(), bufferSize, stride))
        return fail(QStringLiteral("WebP decode failed (truncated or unsupported bitstream)"));
    return image;
}

} // namespace WebPImage
