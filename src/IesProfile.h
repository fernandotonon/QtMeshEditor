#pragma once

#include <QString>
#include <QVector>

namespace Ogre
{
class Light;
}

struct IesProfile
{
    bool valid = false;
    QString sourcePath;
    QVector<float> verticalAnglesDeg;
    QVector<float> candela; ///< flattened LM-63 horizontal-major: index = h * numVertical + v
    float totalLumens = 0.0f;
    float maxCandela = 0.0f;
    float beamAngleDeg = 90.0f; ///< angle where intensity drops to 50% of peak (0° slice)
    float fieldAngleDeg = 120.0f; ///< angle where intensity drops to 10% of peak

    static IesProfile parseFile(const QString& path, QString* error = nullptr);
    static IesProfile parseBytes(const QByteArray& bytes, QString* error = nullptr);
    QVector<float> polarSlice() const; ///< normalized [0..1] candela vs angle for gizmo plot
};

namespace IesLightApply
{

/// Approximate IES distribution on a point/spot light using Ogre spotlight cones.
void applyToLight(const IesProfile& profile, Ogre::Light* light, float basePowerScale = 1.0f);

} // namespace IesLightApply
