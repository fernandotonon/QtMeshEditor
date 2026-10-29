#pragma once

#include "KimodoPredictor.h"

#include <QString>
#include <QStringList>

// `qtmesh anim <rig> --generate "<prompt>" --backend kimodo ...` and
// `qtmesh anim <rig> --apply-motion <motion.npz> ...`: NVIDIA Kimodo text-to-
// motion (or an existing interchange file) retargeted onto the rig, exported.
namespace AIMotion {

struct CliRequest {
    QString rigPath;
    QString outputPath;
    QString prompt;                 ///< single prompt (with duration)
    double duration = 5.0;
    QStringList segments;           ///< "TEXT@SECONDS", one per timeline segment (overrides prompt)
    QStringList applyMotion;        ///< --apply-motion: existing interchange NPZs, no inference
    QString name;                   ///< clip name (default generated_kimodo)
    int variations = 1;
    bool hasSeed = false;
    int seed = 0;
    int diffusionSteps = 0;
    bool rootMotion = true;
    bool footPin = true;
    bool postprocess = true;
    QString constraintsPath;
    QString textEncoderUrl;
    QString textEncoderQuant;       ///< auto|none|int8|int4 (empty = sidecar default)
    QString runtime;                ///< auto|native|python (empty = auto)
    bool download = false;          ///< --download: fetch the native Kimodo models first (explicit request)
    float armSpaceDeg = 0.0f;       ///< #854 arm-space swing applied to every written clip (0 = none)
    bool json = false;
};

/// Parse "TEXT@SECONDS" (the last '@' splits, so text may contain '@').
bool parseSegment(const QString& spec, MotionSegment* out, QString* error);

int runCli(const CliRequest& req);

/// The first Kimodo-only option set on `req` ("--runtime", "--download", ...), or empty. Other backends reject
/// them instead of silently ignoring them.
QString kimodoOnlyOption(const CliRequest& req);

} // namespace AIMotion
