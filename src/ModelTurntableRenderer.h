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
enum class TurntableAxis {
  Y, ///< Orbit in the XZ plane (default product-shot turntable).
  X, ///< Orbit in the YZ plane (spin around model X).
  Z  ///< Orbit in the XY plane (spin around model Z).
};

struct TurntableOptions {
  int width = 512;
  int height = 512;
  int frameCount = 12;
  TurntableAxis axis = TurntableAxis::Y;
  /// Camera angle above the orbit plane, in degrees (see axis).
  float elevationDegrees = 20.0f;
  Ogre::ColourValue background{0.12f, 0.12f, 0.13f, 1.0f};
  /// #936: sample this skeletal animation across its duration instead of
  /// rotating the pose — frame i at time `length * i/(frameCount-1)`, camera
  /// FIXED at the front unless `orbitWithAnimation` combines both.
  QString animationName;
  bool orbitWithAnimation = false;
  /// #936: pose the model at this time (seconds) for every frame — a normal
  /// orbit of a single posed frame (thumbnail of a specific pose). Ignored
  /// when < 0 or when `animationName` is set.
  float atSeconds = -1.0f;
};

class ModelTurntableRenderer
{
public:
  /// Render `frameCount` views around the chosen axis. Returns false on failure.
  static bool renderToImages(const QList<Ogre::Entity *> &entities, const TurntableOptions &options,
                             QList<QImage> *outFrames, QString *errorOut = nullptr);

  /// Destroy turntable camera / RTT resources (safe to call repeatedly).
  static void shutdown();

  /// Lay out equal-sized frames in a single horizontal strip (columns=0 → one row).
  static QImage composeSpriteSheet(const QList<QImage> &frames, int columns = 0);

  /// Parse `--axis` value (`y`, `x`, `z`). Returns false if invalid.
  static bool parseAxis(const QString &text, TurntableAxis *outAxis);
};

#endif // MODELTURNTABLERENDERER_H
