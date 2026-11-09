#pragma once

// Hand shapes for generated motion (Kimodo). Kimodo's model drives no finger joints — every finger holds a relaxed
// rest pose for the whole clip (measured: the thumb/middle end markers move < 2 degrees through "clench fists, then
// open the hands"), so the hands never open or close. This layer writes a hand SHAPE (fist, open, point, grip) onto
// the SOMA-77 finger joints of the motion BEFORE the retarget, so the existing retargeter carries it to any rig
// (named fingers, or UniRig fingers found by the unnamed-finger fallback).
//
// Shapes are absolute curls from SOMA's flat rest hand, about each finger joint's bend axis
// (cross(bone direction, palm normal), the palm normal from the rest hand). "Relaxed" keeps the motion's own hands.
// The shape can change over time (one key per prompt segment) and blends over a few frames at each change.

#include "MotionInterchange.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <vector>

namespace AIMotion::HandShapes {

enum class Shape { Relaxed, Open, Fist, Grip, Point };
QStringList shapeIds();                               ///< "relaxed", "open", "fist", "grip", "point"
bool shapeFromId(const QString& id, Shape* shape);    ///< empty = Relaxed
QString shapeId(Shape s);

/// Which hands a prompt talks about.
struct HandPair {
    Shape left = Shape::Relaxed;
    Shape right = Shape::Relaxed;
};

/// The shape a prompt implies, from its words: fist/punch/clench -> Fist, grab/hold/carry/pick -> Grip,
/// point -> Point, wave/open/spread/high five/clap -> Open; nothing found -> Relaxed. A prompt that names one hand
/// ("with the right hand") shapes that hand only.
HandPair shapeFromPrompt(const QString& prompt);

/// One shape change: from `time` (seconds) on, the hands take this shape.
struct Key {
    double time = 0.0;
    HandPair hands;
};

/// Accepted hands modes: "auto" (the shape each prompt segment implies) or a shape id for the whole clip.
QStringList modeIds();
bool validMode(const QString& mode);   ///< empty = auto

/// The keys for a motion whose metadata (result.json: "segments" [{text, duration}]) names its prompts: one key per
/// segment at its start time. `mode` = "auto" or a shape id (one key at 0). Unknown mode -> empty.
std::vector<Key> keysFor(const QJsonObject& meta, const QString& mode);

/// Writes the keyed shapes into `m`'s finger joints (SOMA-77 names). Each change blends over `blendSeconds`.
/// Returns the number of finger joints written per hand (0 = no SOMA fingers / nothing to do; `why` says why).
int apply(Motion* m, const std::vector<Key>& keys, double blendSeconds = 0.2, QString* why = nullptr);

} // namespace AIMotion::HandShapes
