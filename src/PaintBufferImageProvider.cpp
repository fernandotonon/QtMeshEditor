#include "PaintBufferImageProvider.h"

#include "TexturePaintController.h"

PaintBufferImageProvider::PaintBufferImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage PaintBufferImageProvider::requestImage(
    const QString& id, QSize* size, const QSize& requestedSize)
{
    Q_UNUSED(requestedSize);

    auto* tpc = TexturePaintController::instance();
    if (!tpc) return {};

    QImage img;
    if (id.startsWith(QStringLiteral("layer/"))) {
        const QString idxStr = id.mid(6);
        bool ok = false;
        const int layerIndex = idxStr.toInt(&ok);
        if (ok)
            img = tpc->snapshotLayerImage(layerIndex);
    } else {
        img = tpc->snapshotBufferImage();
    }

    if (size) *size = img.size();
    return img;
}
