#include "CurveEditModel.h"

#include <QString>
#include <QVariant>

#include <cmath>
#include <sstream>

CurveEditModel* CurveEditModel::m_pSingleton = nullptr;

CurveEditModel* CurveEditModel::instance()
{
    if (!m_pSingleton) m_pSingleton = new CurveEditModel(); // NOSONAR — singleton
    return m_pSingleton;
}

CurveEditModel* CurveEditModel::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(scriptEngine)
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void CurveEditModel::kill()
{
    delete m_pSingleton; // NOSONAR — singleton
    m_pSingleton = nullptr;
}

CurveEditModel::CurveEditModel() : QObject(nullptr) {}

std::string CurveEditModel::makeKey(const QString& skeleton,
                                     const QString& anim,
                                     const QString& bone,
                                     const QString& channel,
                                     double time)
{
    // Quantize time to ms so floating-point round-trip doesn't break the key.
    // Same precision as the dope sheet's keyframe-tick comparisons.
    std::ostringstream ss;
    ss << skeleton.toStdString() << '|'
       << anim.toStdString()     << '|'
       << bone.toStdString()     << '|'
       << channel.toStdString()  << '|'
       << static_cast<long long>(std::llround(time * 1000.0));
    return ss.str();
}

bool CurveEditModel::hasEntryForChannel(const QString& skeleton,
                                         const QString& anim,
                                         const QString& bone,
                                         const QString& channel) const
{
    // Composite key prefix: anything matching this (bone, channel) pair
    // regardless of time means the user authored at least one curve
    // handle for it.
    std::ostringstream prefix;
    prefix << skeleton.toStdString() << '|'
           << anim.toStdString()     << '|'
           << bone.toStdString()     << '|'
           << channel.toStdString()  << '|';
    const std::string p = prefix.str();
    for (const auto& [key, entry] : m_entries) {
        if (key.size() >= p.size() && key.compare(0, p.size(), p) == 0)
            return true;
    }
    return false;
}

QVariantList CurveEditModel::tangentsAt(const QString& skeleton,
                                         const QString& anim,
                                         const QString& bone,
                                         const QString& channel,
                                         double time) const
{
    const auto key = makeKey(skeleton, anim, bone, channel, time);
    QVariantList out;
    if (auto it = m_entries.find(key); it != m_entries.end()) {
        out << it->second.inTangent
            << it->second.outTangent
            << static_cast<int>(it->second.mode);
    } else {
        // Default: Bezier with zero tangents (collapses to a hold curve;
        // user-facing visuals make this look like Linear until edited).
        out << 0.0 << 0.0 << static_cast<int>(ModeBezier);
    }
    return out;
}

void CurveEditModel::setTangents(const QString& skeleton,
                                  const QString& anim,
                                  const QString& bone,
                                  const QString& channel,
                                  double time,
                                  double inTangent, double outTangent)
{
    const auto key = makeKey(skeleton, anim, bone, channel, time);
    auto& entry = m_entries[key];
    entry.inTangent  = inTangent;
    entry.outTangent = outTangent;
    if (entry.mode == ModeLinear || entry.mode == ModeStepped) {
        // Editing a tangent implies the user wants curve control —
        // promote the mode to Bezier so the tangents take effect.
        entry.mode = ModeBezier;
    }
    emit modelChanged(skeleton, anim, bone, channel);
}

void CurveEditModel::setMode(const QString& skeleton,
                              const QString& anim,
                              const QString& bone,
                              const QString& channel,
                              double time, int mode)
{
    const auto key = makeKey(skeleton, anim, bone, channel, time);
    auto& entry = m_entries[key];
    if (mode == ModeBezier || mode == ModeLinear ||
        mode == ModeStepped || mode == ModeAuto) {
        entry.mode = static_cast<InterpMode>(mode);
        emit modelChanged(skeleton, anim, bone, channel);
    }
}

void CurveEditModel::clearAnimation(const QString& skeleton, const QString& anim)
{
    const std::string prefix =
        skeleton.toStdString() + "|" + anim.toStdString() + "|";
    for (auto it = m_entries.begin(); it != m_entries.end(); ) {
        if (it->first.rfind(prefix, 0) == 0) it = m_entries.erase(it);
        else                                  ++it;
    }
}

double CurveEditModel::evaluate(const QString& skeleton,
                                 const QString& anim,
                                 const QString& bone,
                                 const QString& channel,
                                 double time,
                                 const QVariantList& keyframeTimes,
                                 const QVariantList& keyframeValues) const
{
    const auto n = static_cast<int>(keyframeTimes.size());
    if (n == 0 || n != static_cast<int>(keyframeValues.size())) return 0.0;
    // Single-keyframe tracks degenerate to a constant.
    if (n == 1) return keyframeValues.first().toDouble();

    // Clamp before the first keyframe — otherwise the bracket [t0, t1]
    // would be evaluated with negative u, extrapolating outside the
    // curve. Holding the first value matches Ogre's playback semantics.
    if (time <= keyframeTimes.first().toDouble()) {
        return keyframeValues.first().toDouble();
    }

    // Locate the bracketing pair (i, i+1) such that t[i] <= time < t[i+1].
    int lo = 0;
    while (lo + 1 < n && keyframeTimes[lo + 1].toDouble() <= time) ++lo;
    if (lo == n - 1) return keyframeValues[n - 1].toDouble();
    const int hi = lo + 1;

    const double tLo = keyframeTimes[lo].toDouble();
    const double tHi = keyframeTimes[hi].toDouble();
    const double vLo = keyframeValues[lo].toDouble();
    const double vHi = keyframeValues[hi].toDouble();
    const double dt  = tHi - tLo;
    if (dt <= 0.0) return vLo;
    const double u = (time - tLo) / dt; // [0, 1]

    // Default mode = Bezier with 0 tangents → behaves like a flat curve
    // (vLo to vHi via a hold-then-jump). Linear and Stepped are explicit.
    Entry loEntry{};
    if (auto it = m_entries.find(makeKey(skeleton, anim, bone, channel, tLo));
        it != m_entries.end()) {
        loEntry = it->second;
    }

    if (loEntry.mode == ModeStepped) return vLo;
    if (loEntry.mode == ModeLinear)  return vLo + (vHi - vLo) * u;

    // Bezier or Auto. For Auto, derive Catmull-Rom-style tangents from
    // neighbors; for Bezier, use the stored handles directly (or 0, which
    // collapses to hold-and-jump until the user drags a handle).
    double tangentOut = loEntry.outTangent;
    double tangentIn  = 0.0;
    if (auto it = m_entries.find(makeKey(skeleton, anim, bone, channel, tHi));
        it != m_entries.end()) {
        tangentIn = it->second.inTangent;
    }
    if (loEntry.mode == ModeAuto) {
        // Catmull-Rom-style auto tangent at point i =
        //     (value[i+1] - value[i-1]) / (time[i+1] - time[i-1])
        // With uniform spacing this collapses to the simple half-difference,
        // but non-uniform keyframes need the actual time span in the
        // denominator so Hermite's `tangent * dt` term scales correctly.
        const double tPrev = (lo > 0)
            ? keyframeTimes[lo - 1].toDouble() : tLo;
        const double vPrev = (lo > 0)
            ? keyframeValues[lo - 1].toDouble() : vLo;
        const double tNext = (hi + 1 < n)
            ? keyframeTimes[hi + 1].toDouble() : tHi;
        const double vNext = (hi + 1 < n)
            ? keyframeValues[hi + 1].toDouble() : vHi;
        const double spanLo = tHi - tPrev;
        const double spanHi = tNext - tLo;
        tangentOut = (spanLo > 0.0) ? (vHi - vPrev) / spanLo : 0.0;
        tangentIn  = (spanHi > 0.0) ? (vNext - vLo) / spanHi : 0.0;
    }

    // Cubic Hermite spline (equivalent to Bezier with derived control
    // points, parameterized by tangents at the endpoints).
    const double u2 = u * u;
    const double u3 = u2 * u;
    const double h00 =  2.0 * u3 - 3.0 * u2 + 1.0;
    const double h10 =        u3 - 2.0 * u2 + u;
    const double h01 = -2.0 * u3 + 3.0 * u2;
    const double h11 =        u3 -       u2;
    return h00 * vLo + h10 * dt * tangentOut +
           h01 * vHi + h11 * dt * tangentIn;
}
