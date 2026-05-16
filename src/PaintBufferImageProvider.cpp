#include "PaintBufferImageProvider.h"

#include "TexturePaintController.h"

PaintBufferImageProvider::PaintBufferImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage PaintBufferImageProvider::requestImage(
    const QString& id, QSize* size, const QSize& requestedSize)
{
    Q_UNUSED(id);            // We only have one "image" — the live buffer.
    Q_UNUSED(requestedSize); // QML's sourceSize hint is honoured by Image{}.

    auto* tpc = TexturePaintController::instance();
    if (!tpc) return {};
    QImage img = tpc->snapshotBufferImage();
    if (size) *size = img.size();
    return img;
}
