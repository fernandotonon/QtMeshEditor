#ifndef MODELTURNTABLERENDERER_H
#define MODELTURNTABLERENDERER_H

#include <QImage>
#include <QString>
#include <QList>

#include <Ogre.h>

/**
 * Headless Ogre render-to-texture turntable for mesh previews (#294).
 *
 * Orbits a camera around the combined world bounding box of one or more
 * entities and returns RGBA frames suitable for PNG export (CLI / batch).
 */
struct TurntableOptions {
    int width = 512;
    int height = 512;
    int frameCount = 12;
    float elevationDegrees = 20.0f;
    Ogre::ColourValue background{0.12f, 0.12f, 0.13f, 1.0f};
};

class ModelTurntableRenderer
{
public:
    /// Render `frameCount` views around the Y axis. Returns false on failure.
    static bool renderToImages(const QList<Ogre::Entity *> &entities, const TurntableOptions &options,
                               QList<QImage> *outFrames, QString *errorOut = nullptr);

    /// Destroy turntable camera / RTT resources (safe to call repeatedly).
    static void shutdown();

    /// Lay out equal-sized frames in a single horizontal strip (columns=0 → one row).
    static QImage composeSpriteSheet(const QList<QImage> &frames, int columns = 0);
};

#endif // MODELTURNTABLERENDERER_H
