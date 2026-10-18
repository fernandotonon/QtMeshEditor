#include "DnaArkitMap.h"

#include "../Mocap/FaceCapCanonicalData.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace Dna {
namespace ArkitMap {

QString controlPrefix() { return QStringLiteral("CTRL_expressions."); }

namespace {

using C = QList<QPair<QString, double>>;

C one(const char* a) { return {{QString::fromLatin1(a), 1.0}}; }
C two(const char* a, const char* b) { return {{QString::fromLatin1(a), 1.0}, {QString::fromLatin1(b), 1.0}}; }
C four(const char* a, const char* b, const char* c, const char* d)
{
    return {{QString::fromLatin1(a), 1.0}, {QString::fromLatin1(b), 1.0},
            {QString::fromLatin1(c), 1.0}, {QString::fromLatin1(d), 1.0}};
}

Entry E(const char* arkit, C controls, const char* note = nullptr)
{
    Entry e;
    e.arkit = QString::fromLatin1(arkit);
    e.controls = std::move(controls);
    e.approximate = note != nullptr;
    if (note) e.note = QString::fromLatin1(note);
    return e;
}

}  // namespace

QList<Entry> defaultMap()
{
    // MetaHuman's L / R are the CHARACTER's left / right, like ARKit's Left /
    // Right, so sides map straight across. ARKit "In" / "Out" are relative to
    // the nose, so the left eye looking IN is MetaHuman's left eye looking RIGHT.
    return {
        E("browDownLeft", one("browDownL")),
        E("browDownRight", one("browDownR")),
        E("browInnerUp", two("browRaiseInL", "browRaiseInR")),
        E("browOuterUpLeft", one("browRaiseOuterL")),
        E("browOuterUpRight", one("browRaiseOuterR")),
        E("cheekPuff", two("mouthCheekBlowL", "mouthCheekBlowR")),
        E("cheekSquintLeft", one("eyeCheekRaiseL"), "MetaHuman's cheek raise also lifts the lower lid"),
        E("cheekSquintRight", one("eyeCheekRaiseR"), "MetaHuman's cheek raise also lifts the lower lid"),
        E("eyeBlinkLeft", one("eyeBlinkL")),
        E("eyeBlinkRight", one("eyeBlinkR")),
        E("eyeLookDownLeft", one("eyeLookDownL")),
        E("eyeLookDownRight", one("eyeLookDownR")),
        E("eyeLookInLeft", one("eyeLookRightL")),
        E("eyeLookInRight", one("eyeLookLeftR")),
        E("eyeLookOutLeft", one("eyeLookLeftL")),
        E("eyeLookOutRight", one("eyeLookRightR")),
        E("eyeLookUpLeft", one("eyeLookUpL")),
        E("eyeLookUpRight", one("eyeLookUpR")),
        E("eyeSquintLeft", one("eyeSquintInnerL"), "closest MetaHuman squint control"),
        E("eyeSquintRight", one("eyeSquintInnerR"), "closest MetaHuman squint control"),
        E("eyeWideLeft", one("eyeWidenL")),
        E("eyeWideRight", one("eyeWidenR")),
        E("jawForward", one("jawFwd")),
        E("jawLeft", one("jawLeft")),
        E("jawOpen", one("jawOpen")),
        E("jawRight", one("jawRight")),
        E("mouthClose", four("mouthLipsTogetherUL", "mouthLipsTogetherUR", "mouthLipsTogetherDL", "mouthLipsTogetherDR"),
          "ARKit's counter-shape to jawOpen; MetaHuman closes the lips with lips-together"),
        E("mouthDimpleLeft", one("mouthDimpleL")),
        E("mouthDimpleRight", one("mouthDimpleR")),
        E("mouthFrownLeft", one("mouthCornerDepressL")),
        E("mouthFrownRight", one("mouthCornerDepressR")),
        E("mouthFunnel", four("mouthFunnelUL", "mouthFunnelUR", "mouthFunnelDL", "mouthFunnelDR"),
          "MetaHuman splits the funnel into four lip quadrants"),
        E("mouthLeft", one("mouthLeft")),
        E("mouthLowerDownLeft", one("mouthLowerLipDepressL")),
        E("mouthLowerDownRight", one("mouthLowerLipDepressR")),
        E("mouthPressLeft", two("mouthPressUL", "mouthPressDL")),
        E("mouthPressRight", two("mouthPressUR", "mouthPressDR")),
        E("mouthPucker", four("mouthLipsPurseUL", "mouthLipsPurseUR", "mouthLipsPurseDL", "mouthLipsPurseDR"),
          "MetaHuman splits the pucker into four lip quadrants"),
        E("mouthRight", one("mouthRight")),
        E("mouthRollLower", two("mouthLowerLipRollInL", "mouthLowerLipRollInR"), "both sides of the lower-lip roll"),
        E("mouthRollUpper", two("mouthUpperLipRollInL", "mouthUpperLipRollInR"), "both sides of the upper-lip roll"),
        E("mouthShrugLower", two("jawChinRaiseDL", "jawChinRaiseDR"), "chin raise pushes the lower lip up"),
        E("mouthShrugUpper", {{QStringLiteral("mouthUpperLipRaiseL"), 0.5}, {QStringLiteral("mouthUpperLipRaiseR"), 0.5}},
          "half-strength upper-lip raise"),
        E("mouthSmileLeft", one("mouthCornerPullL")),
        E("mouthSmileRight", one("mouthCornerPullR")),
        E("mouthStretchLeft", one("mouthStretchL")),
        E("mouthStretchRight", one("mouthStretchR")),
        E("mouthUpperUpLeft", one("mouthUpperLipRaiseL")),
        E("mouthUpperUpRight", one("mouthUpperLipRaiseR")),
        E("noseSneerLeft", one("noseWrinkleL")),
        E("noseSneerRight", one("noseWrinkleR")),
    };
}

LoadResult loadOverride(const QString& jsonPath)
{
    LoadResult r;
    r.entries = defaultMap();
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        r.error = QStringLiteral("cannot read ARKit map '%1'").arg(jsonPath);
        return r;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!doc.isObject()) {
        r.error = QStringLiteral("'%1' is not a JSON object (%2)").arg(jsonPath, pe.errorString());
        return r;
    }
    const QJsonObject root = doc.object();
    if (root.contains(QStringLiteral("schema"))
        && root.value(QStringLiteral("schema")).toString() != QLatin1String("qtmesh-dna-arkit-map-v1")) {
        r.error = QStringLiteral("unsupported ARKit map schema '%1'").arg(root.value(QStringLiteral("schema")).toString());
        return r;
    }
    QSet<QString> canonical;
    for (int i = 1; i < FaceCap::kBlendshapeCount; ++i)
        canonical.insert(QString::fromLatin1(FaceCap::kBlendshapeNames[size_t(i)]));
    if (!root.value(QStringLiteral("map")).isObject()) {
        r.error = QStringLiteral("'%1' has no \"map\" object (a misspelled top-level key would otherwise "
                                 "silently apply nothing)").arg(jsonPath);
        return r;
    }
    const QJsonObject map = root.value(QStringLiteral("map")).toObject();
    for (auto it = map.begin(); it != map.end(); ++it) {
        if (!canonical.contains(it.key())) {
            r.error = QStringLiteral("'%1' is not an ARKit channel name").arg(it.key());
            return r;
        }
        if (!it.value().isObject()) {
            r.error = QStringLiteral("map entry '%1' must be an object of control: weight").arg(it.key());
            return r;
        }
        Entry e;
        e.arkit = it.key();
        const QJsonObject ctrls = it.value().toObject();
        for (auto c = ctrls.begin(); c != ctrls.end(); ++c) {
            if (!c.value().isDouble() || !std::isfinite(c.value().toDouble())) {
                r.error = QStringLiteral("weight for '%1' in '%2' must be a finite number").arg(c.key(), it.key());
                return r;
            }
            e.controls.append({c.key(), c.value().toDouble()});
        }
        e.note = QStringLiteral("project override");
        for (Entry& d : r.entries)
            if (d.arkit == e.arkit) { d = e; break; }
        (e.controls.isEmpty() ? r.dropped : r.overridden) << e.arkit;
    }
    r.entries.erase(std::remove_if(r.entries.begin(), r.entries.end(),
                                   [](const Entry& e) { return e.controls.isEmpty(); }),
                    r.entries.end());
    r.ok = true;
    return r;
}

QStringList exactChannels(const QList<Entry>& map)
{
    QStringList out;
    for (const Entry& e : map) if (!e.approximate) out << e.arkit;
    return out;
}

QStringList approximateChannels(const QList<Entry>& map)
{
    QStringList out;
    for (const Entry& e : map) if (e.approximate) out << e.arkit;
    return out;
}

}  // namespace ArkitMap
}  // namespace Dna
