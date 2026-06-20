#ifndef MODELISOMETRICRENDERER_H
#define MODELISOMETRICRENDERER_H

#include "ModelTurntableRenderer.h"

#include <QImage>
#include <QString>
#include <QList>

#include <Ogre.h>

/**
 * Headless Ogre render-to-texture isometric / 8-direction sprite export (#724).
 *
 * Renders a model from fixed compass directions while optionally sampling an
 * animation into N frames per direction. Output layout: rows = directions,
 * columns = animation frames (isometric game sprite atlas).
 */
struct IsometricOptions {
  int width = 512;
  int height = 512;
  /// Camera angle above the orbit plane, in degrees (~30° isometric default).
  float elevationDegrees = 30.0f;
  TurntableAxis upAxis = TurntableAxis::Y;
  Ogre::ColourValue background{0.12f, 0.12f, 0.13f, 1.0f};
  int directionCount = 8;
  /// Align row 0 to the game's facing direction (degrees).
  float startAzimuthDegrees = 0.0f;
  /// Fixed orbit distance in world units. 0 = auto-fit from bounds (default).
  float cameraDistance = 0.0f;
  /// Multiplier on auto-fit distance when cameraDistance is 0 (default 1.25).
  float cameraPadding = 1.25f;
};

class ModelIsometricRenderer
{
public:
  /// Human-readable direction row order for CLI/MCP reports.
  static QString directionOrderConvention();

  /// Render a directions × frames grid. Returns false on failure.
  static bool renderToGrid(const QList<Ogre::Entity *> &entities, Ogre::Entity *animatedEntity,
                           const QString &animationName, int frameCount, const IsometricOptions &options,
                           QList<QList<QImage>> *outRowsByDirection, QString *errorOut = nullptr);

  /// Lay out rows (directions) × columns (frames) into a single atlas image.
  static QImage composeDirectionGrid(const QList<QList<QImage>> &rowsByDirection);

  /// Destroy isometric camera / RTT resources (safe to call repeatedly).
  static void shutdown();

  /// First skinned entity that owns `animationName`, or nullptr.
  static Ogre::Entity *findEntityWithAnimation(const QList<Ogre::Entity *> &entities,
                                               const QString &animationName);

  /// Multi-line listing of `[entity] clip` rows for CLI/MCP error hints.
  static QString formatAvailableAnimations(const QList<Ogre::Entity *> &entities);
};

#endif // MODELISOMETRICRENDERER_H
