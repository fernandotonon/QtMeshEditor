#ifndef CURVE_EDIT_MODEL_H
#define CURVE_EDIT_MODEL_H

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QVariantList>
#include <unordered_map>
#include <string>

/**
 * Side-table for Bezier-style curve editing on top of Ogre's
 * TransformKeyFrames. Ogre's keyframe class stores T/R/S at a time but
 * has no slot for tangent handles or per-keyframe interpolation modes.
 * Storing those here, keyed by (skeleton, animation, bone, channel, time),
 * lets the curve editor render Bezier curves and re-sample them into the
 * underlying TransformKeyFrames on edit.
 *
 * Channel ids match the dope sheet:
 *   tx ty tz   rw rx ry rz   sx sy sz
 *
 * Modes:
 *   Bezier  — explicit in/out tangent handles (default for new entries)
 *   Linear  — straight line between this keyframe and the next
 *   Stepped — hold value until the next keyframe
 *   Auto    — Catmull-Rom-style automatic tangents from neighbors
 *
 * The model is in-memory only in this slice. Persistence (e.g. a sidecar
 * JSON file alongside the scene) is a separate issue (#378 follow-up).
 */
class CurveEditModel : public QObject
{
    Q_OBJECT

public:
    enum InterpMode {
        ModeBezier  = 0,
        ModeLinear  = 1,
        ModeStepped = 2,
        ModeAuto    = 3
    };
    Q_ENUM(InterpMode)

    static CurveEditModel* instance();
    static CurveEditModel* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    /// Read the (in, out, mode) for a specific keyframe channel.
    /// Returns a default {0, 0, Bezier} when not set.
    Q_INVOKABLE QVariantList tangentsAt(const QString& skeleton,
                                        const QString& anim,
                                        const QString& bone,
                                        const QString& channel,
                                        double time) const;

    /// Update tangent handles. Bezier mode is set if not already.
    Q_INVOKABLE void setTangents(const QString& skeleton,
                                 const QString& anim,
                                 const QString& bone,
                                 const QString& channel,
                                 double time,
                                 double inTangent, double outTangent);

    Q_INVOKABLE void setMode(const QString& skeleton,
                             const QString& anim,
                             const QString& bone,
                             const QString& channel,
                             double time,
                             int mode);

    /// Drop every entry for an entire animation (called when the animation
    /// is renamed or removed).
    Q_INVOKABLE void clearAnimation(const QString& skeleton,
                                    const QString& anim);

    /// Evaluate the curve at `time` for one channel. Returns the stored
    /// keyframe value at `time` if there's an exact match; otherwise
    /// interpolates per the upstream keyframe's outgoing mode.
    /// `keyframeTimes` and `keyframeValues` are the channel's authoritative
    /// data from Ogre — passed in instead of resolved here so this method
    /// stays free of Ogre dependencies and remains pure-data testable.
    Q_INVOKABLE double evaluate(const QString& skeleton,
                                const QString& anim,
                                const QString& bone,
                                const QString& channel,
                                double time,
                                const QVariantList& keyframeTimes,
                                const QVariantList& keyframeValues) const;

signals:
    void modelChanged(const QString& skeleton,
                      const QString& anim,
                      const QString& bone,
                      const QString& channel);

private:
    CurveEditModel();
    ~CurveEditModel() override = default;

    struct Entry {
        double     inTangent  = 0.0;
        double     outTangent = 0.0;
        InterpMode mode       = ModeBezier;
    };

    /// Composite key as a single string; cheap to hash and easy to compose.
    static std::string makeKey(const QString& skeleton,
                               const QString& anim,
                               const QString& bone,
                               const QString& channel,
                               double time);

    static CurveEditModel* m_pSingleton;
    std::unordered_map<std::string, Entry> m_entries;
};

#endif // CURVE_EDIT_MODEL_H
