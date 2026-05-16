#ifndef PAINTBUFFERIMAGEPROVIDER_H
#define PAINTBUFFERIMAGEPROVIDER_H

#include <QQuickImageProvider>

/**
 * @brief QQuickImageProvider that serves the live texture paint buffer.
 *
 * Registered in `main.cpp` under the URL scheme `image://paintbuffer/...`.
 * The detached Texture Editor window binds its `Image` element to a URL
 * produced by `TexturePaintController::fullResPreviewUrl()`, which
 * includes a monotonic `?v=N` so QML's Image cache re-fetches on every
 * paint refresh.
 *
 * The implementation just calls `TexturePaintController::instance()
 * ->snapshotBufferImage()` and hands the result back. That's a single
 * `QImage::copy()` of the RGBA buffer — no PNG encode, no base64,
 * no megabyte-sized QString churn. On a 2048² texture this is ~3 ms
 * vs ~80 ms for the data-URI path the inspector thumbnail uses.
 */
class PaintBufferImageProvider : public QQuickImageProvider
{
public:
    PaintBufferImageProvider();

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
};

#endif // PAINTBUFFERIMAGEPROVIDER_H
