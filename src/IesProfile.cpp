#include "IesProfile.h"

#include <OgreLight.h>
#include <OgreMath.h>

#include <QFile>
#include <QRegularExpression>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace
{

float angleAtFraction(const QVector<float>& angles, const QVector<float>& values, float fraction)
{
    if (angles.isEmpty() || values.isEmpty() || angles.size() != values.size())
        return 90.0f;

    const float peak = *std::max_element(values.constBegin(), values.constEnd());
    if (peak <= 0.0f)
        return 90.0f;

    const float target = peak * fraction;
    for (int i = 0; i < values.size(); ++i)
    {
        if (values[i] >= target)
            continue;
        if (i == 0)
            return angles[0];
        const float t = (target - values[i - 1]) / (values[i] - values[i - 1]);
        return angles[i - 1] + t * (angles[i] - angles[i - 1]);
    }
    return angles.last();
}

QVector<QString> tokenizeLines(const QByteArray& bytes)
{
    QVector<QString> lines;
    const QList<QByteArray> raw = bytes.split('\n');
    lines.reserve(raw.size());
    for (const QByteArray& line : raw)
        lines.append(QString::fromUtf8(line.trimmed()));
    return lines;
}

} // namespace

IesProfile IesProfile::parseBytes(const QByteArray& bytes, QString* error)
{
    IesProfile profile;

    const QVector<QString> lines = tokenizeLines(bytes);
    int tiltLine = -1;
    for (int i = 0; i < lines.size(); ++i)
    {
        if (lines[i].startsWith(QStringLiteral("TILT"), Qt::CaseInsensitive))
        {
            tiltLine = i;
            break;
        }
    }
    if (tiltLine < 0)
    {
        if (error)
            *error = QStringLiteral("Missing TILT keyword");
        return profile;
    }

    int cursor = tiltLine + 1;
    auto readFloats = [&](int count) -> QVector<float> {
        QVector<float> out;
        out.reserve(count);
        while (out.size() < count && cursor < lines.size())
        {
            const QString line = lines[cursor++].trimmed();
            if (line.isEmpty())
                continue;

            const QStringList parts =
                line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            for (const QString& part : parts)
            {
                bool ok = false;
                const float value = part.toFloat(&ok);
                if (ok)
                    out.append(value);
                if (out.size() >= count)
                    break;
            }
        }
        return out;
    };

    const QVector<float> header = readFloats(10);
    if (header.size() < 10)
    {
        if (error)
            *error = QStringLiteral("Truncated IES header");
        return profile;
    }

    const int numVertical = static_cast<int>(header[3]);
    const int numHorizontal = static_cast<int>(header[4]);
    if (numVertical <= 0 || numHorizontal <= 0)
    {
        if (error)
            *error = QStringLiteral("Invalid IES angle counts");
        return profile;
    }

    readFloats(5); // skip electrical + multipliers we don't need for v1

    const QVector<float> vertical = readFloats(numVertical);
    const QVector<float> horizontal = readFloats(numHorizontal);
    const QVector<float> candelaFlat = readFloats(numVertical * numHorizontal);

    if (vertical.size() != numVertical || candelaFlat.size() != numVertical * numHorizontal)
    {
        if (error)
            *error = QStringLiteral("Truncated IES candela table");
        return profile;
    }

    Q_UNUSED(horizontal);

    profile.verticalAnglesDeg = vertical;
    profile.candela = candelaFlat;
    profile.maxCandela = 0.0f;
    for (float c : candelaFlat)
        profile.maxCandela = std::max(profile.maxCandela, c);

    QVector<float> slice0;
    slice0.reserve(numVertical);
    for (int v = 0; v < numVertical; ++v)
        slice0.append(candelaFlat[v]); // horizontal index 0 → candela[h * numVertical + v]

    profile.beamAngleDeg = angleAtFraction(vertical, slice0, 0.5f);
    profile.fieldAngleDeg = angleAtFraction(vertical, slice0, 0.1f);
    profile.totalLumens = header[1];
    profile.valid = profile.maxCandela > 0.0f;
    if (!profile.valid && error)
        *error = QStringLiteral("IES candela table is empty");
    return profile;
}

IesProfile IesProfile::parseFile(const QString& path, QString* error)
{
    IesProfile profile;
    profile.sourcePath = path;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error)
            *error = QStringLiteral("Cannot open IES file");
        return profile;
    }

    profile = parseBytes(file.readAll(), error);
    profile.sourcePath = path;
    return profile;
}

QVector<float> IesProfile::polarSlice() const
{
    QVector<float> out;
    if (!valid || verticalAnglesDeg.isEmpty())
        return out;

    const int numVertical = verticalAnglesDeg.size();
    out.reserve(numVertical);
    for (int v = 0; v < numVertical; ++v)
    {
        const float c = candela[v]; // horizontal index 0
        out.append(maxCandela > 0.0f ? c / maxCandela : 0.0f);
    }
    return out;
}

namespace IesLightApply
{

void applyToLight(const IesProfile& profile, Ogre::Light* light, float basePowerScale)
{
    if (!profile.valid || !light)
        return;

    const float inner = qBound(1.0f, profile.beamAngleDeg * 0.65f, 179.0f);
    const float outer = qBound(inner + 1.0f, profile.fieldAngleDeg, 179.0f);
    const float falloff = qBound(0.1f, profile.beamAngleDeg / qMax(1.0f, profile.fieldAngleDeg), 4.0f);

    if (light->getType() == Ogre::Light::LT_SPOTLIGHT)
    {
        light->setSpotlightRange(Ogre::Degree(inner), Ogre::Degree(outer), falloff);
    }
    else if (light->getType() == Ogre::Light::LT_POINT)
    {
        light->setType(Ogre::Light::LT_SPOTLIGHT);
        light->setSpotlightRange(Ogre::Degree(inner), Ogre::Degree(outer), falloff);
    }

    const float reference = 1000.0f;
    const float scale = qBound(0.05f, profile.maxCandela / reference, 8.0f);
    light->setPowerScale(basePowerScale * scale);
}

} // namespace IesLightApply
