#pragma once

#include <QColor>
#include <QString>

namespace HdrMaterialScript {

constexpr float kDefaultEnvIntensity = 1.f;
constexpr float kMinEnvIntensity = 0.f;
constexpr float kMaxEnvIntensity = 4.f;

/// Strip `pbr_environment_*` lines before feeding the script to Ogre.
QString stripEnvironmentLines(const QString& materialScript);

/// Insert or replace environment lines inside the first `pass` block.
QString injectEnvironmentLines(const QString& materialScript,
                               float intensity,
                               const QColor& tint);

/// Parse environment lines from a material script (defaults if absent).
bool parseEnvironmentLines(const QString& materialScript,
                           float& intensityOut,
                           QColor& tintOut);

} // namespace HdrMaterialScript
