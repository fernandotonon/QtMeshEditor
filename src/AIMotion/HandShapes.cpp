#include "HandShapes.h"

#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <cmath>

namespace AIMotion::HandShapes {

namespace {

// Curl (degrees) of the three bending joints of a finger: knuckle (MCP), middle (PIP), tip (DIP); for the thumb its
// three joints from the base. Measured against SOMA's flat rest hand.
struct Curl {
    std::array<float, 3> finger;
    std::array<float, 3> thumb;
};

Curl curlOf(Shape s)
{
    switch (s) {
    // Thumb angles go with kThumbMix: chosen on close-up renders of a fist on a generated character (the old
    // into-the-palm thumb stuck straight out sideways; these tuck it over the index and middle fingers).
    case Shape::Fist: return {{85.0f, 100.0f, 65.0f}, {20.0f, 42.0f, 42.0f}};
    case Shape::Grip: return {{50.0f, 60.0f, 35.0f}, {10.0f, 22.0f, 22.0f}};
    case Shape::Point: return {{85.0f, 100.0f, 65.0f}, {20.0f, 42.0f, 42.0f}};   // index overridden below
    case Shape::Open: return {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    case Shape::Relaxed: break;
    }
    return {};
}

// Thumb flexion axis per joint (base, middle, tip): 0 = across the palm (about the palm normal), 1 = into the palm.
constexpr std::array<float, 3> kThumbMix = {0.65f, 0.2f, 0.2f};

constexpr std::array<const char*, 4> kLongFingers = {"Index", "Middle", "Ring", "Pinky"};

// One finger joint the layer writes: its index and, per shape, its absolute local rotation (identity = flat rest).
struct Joint {
    int index = -1;
    Ogre::Vector3 axis = Ogre::Vector3::UNIT_X;   ///< bend axis in the (rest-aligned) parent frame
    Ogre::Vector3 across = Ogre::Vector3::UNIT_Y;   ///< thumb: the axis that swings it across the palm toward the little finger
    int segment = 0;                              ///< 0..2 along the finger
    bool thumb = false;
    bool index_ = false;                          ///< the index finger (Point keeps it straight)
    int metacarpal = -1;                          ///< long fingers: SOMA's metacarpal joint (straightened with the shape)
};

struct Hand {
    std::vector<Joint> joints;
    std::vector<int> metacarpals;
};

Ogre::Vector3 rest(const Motion& m, const std::string& name)
{
    const int i = m.indexOf(name);
    return i >= 0 ? m.restJoints[size_t(i)] : Ogre::Vector3::ZERO;
}

// The bending joints of one hand, with bend axes from the rest pose. Empty when the motion has no SOMA fingers.
Hand handOf(const Motion& m, const std::string& side)
{
    Hand h;
    const std::string hand = side + "Hand";
    for (const std::string n : {hand, hand + "Middle2", hand + "Index2", hand + "Pinky2", hand + "Thumb2"})
        if (m.indexOf(n) < 0) return h;
    const Ogre::Vector3 along = rest(m, hand + "Middle2") - rest(m, hand);
    const Ogre::Vector3 across = rest(m, hand + "Pinky2") - rest(m, hand + "Index2");
    Ogre::Vector3 palm = along.crossProduct(across).normalisedCopy();
    // The palm side is where the thumb sits: flip the normal so the thumb base is on its positive side.
    if (palm.dotProduct(rest(m, hand + "Thumb2") - rest(m, hand)) < 0.0f) palm = -palm;
    const Ogre::Vector3 toPinky = rest(m, hand + "Pinky2") - rest(m, hand + "Index2");

    const auto addChain = [&](const std::string& base, int first, bool thumb, bool indexFinger) {
        for (int seg = 0; seg < 3; ++seg) {
            const std::string name = base + std::to_string(first + seg);
            const int j = m.indexOf(name);
            const int child = m.indexOf(seg < 2 ? base + std::to_string(first + seg + 1) : base + "End");
            if (j < 0 || child < 0) return;
            const Ogre::Vector3 bone = (m.restJoints[size_t(child)] - m.restJoints[size_t(j)]).normalisedCopy();
            Joint jt;
            jt.index = j;
            jt.axis = bone.crossProduct(palm).normalisedCopy();   // rotating the bone about it moves it toward the palm
            // The swing across the palm is about the palm normal, in the sense that moves the bone toward the
            // little finger: decided from the geometry, because a left hand mirrors the right one.
            jt.across = palm.crossProduct(bone).dotProduct(toPinky) >= 0.0f ? palm : -palm;
            jt.segment = seg;
            jt.thumb = thumb;
            jt.index_ = indexFinger;
            h.joints.push_back(jt);
        }
    };
    for (const char* f : kLongFingers) {
        const std::string base = hand + f;
        addChain(base, 2, false, std::string(f) == "Index");
        if (const int mc = m.indexOf(base + "1"); mc >= 0) h.metacarpals.push_back(mc);
    }
    addChain(hand + "Thumb", 1, true, false);
    return h;
}

// The absolute local rotation a shape gives a joint; `relaxed` is the motion's own rotation (kept for Relaxed).
Ogre::Quaternion target(const Joint& j, Shape s, const Ogre::Quaternion& relaxed)
{
    if (s == Shape::Relaxed) return relaxed;
    const Curl c = curlOf(s);
    float deg = j.thumb ? c.thumb[size_t(j.segment)] : c.finger[size_t(j.segment)];
    if (s == Shape::Point && j.index_) deg = 0.0f;
    if (!j.thumb) return Ogre::Quaternion(Ogre::Degree(deg), j.axis);
    // The thumb does not fold into the palm like a finger: its joints flex ACROSS the palm, toward the little
    // finger (a rotation about the palm normal: SOMA's rest thumb lies parallel to the fingers), the base joint
    // also bringing it in front of the palm (opposition). Mix per joint: 0 = across, 1 = into the palm.
    const float mix = kThumbMix[size_t(j.segment)];
    const Ogre::Vector3 axis = (j.across * (1.0f - mix) + j.axis * mix).normalisedCopy();
    return Ogre::Quaternion(Ogre::Degree(deg), axis);
}

// The shape in force at time t and, while blending, the one before it with the blend weight of the new one.
struct ShapeAt {
    HandPair from;
    HandPair to;
    float weight = 1.0f;
};

ShapeAt shapeAt(const std::vector<Key>& keys, double t, double blend)
{
    ShapeAt s;
    for (size_t k = 0; k < keys.size(); ++k) {
        if (keys[k].time > t) break;
        s.from = k == 0 ? keys[k].hands : keys[k - 1].hands;
        s.to = keys[k].hands;
        s.weight = (k == 0 || blend <= 0.0) ? 1.0f : float(std::clamp((t - keys[k].time) / blend, 0.0, 1.0));
    }
    return s;
}

void writeHand(Motion* m, const Hand& hand, Shape from, Shape to, float w, int frame)
{
    std::vector<Ogre::Quaternion>& rot = m->localRot[size_t(frame)];
    for (const Joint& j : hand.joints) {
        const Ogre::Quaternion own = rot[size_t(j.index)];
        rot[size_t(j.index)] = Ogre::Quaternion::Slerp(w, target(j, from, own), target(j, to, own), true);
    }
    // The metacarpals carry part of the relaxed curl: a shape starts from the flat rest hand.
    for (const int mc : hand.metacarpals) {
        const Ogre::Quaternion own = rot[size_t(mc)];
        const Ogre::Quaternion a = from == Shape::Relaxed ? own : Ogre::Quaternion::IDENTITY;
        const Ogre::Quaternion b = to == Shape::Relaxed ? own : Ogre::Quaternion::IDENTITY;
        rot[size_t(mc)] = Ogre::Quaternion::Slerp(w, a, b, true);
    }
}

bool anyShape(const std::vector<Key>& keys)
{
    return std::any_of(keys.begin(), keys.end(),
                       [](const Key& k) { return k.hands.left != Shape::Relaxed || k.hands.right != Shape::Relaxed; });
}

} // namespace

QStringList shapeIds()
{
    return {QStringLiteral("relaxed"), QStringLiteral("open"), QStringLiteral("fist"), QStringLiteral("grip"), QStringLiteral("point")};
}

bool shapeFromId(const QString& id, Shape* shape)
{
    static const Shape kShapes[] = {Shape::Relaxed, Shape::Open, Shape::Fist, Shape::Grip, Shape::Point};
    const QString s = id.trimmed().toLower();
    if (s.isEmpty()) {
        *shape = Shape::Relaxed;
        return true;
    }
    const qsizetype i = shapeIds().indexOf(s);
    if (i < 0) return false;
    *shape = kShapes[i];
    return true;
}

QString shapeId(Shape s) { return shapeIds().at(int(s)); }

HandPair shapeFromPrompt(const QString& prompt)
{
    const QString p = prompt.toLower();
    const auto has = [&p](const char* pattern) { return p.contains(QRegularExpression(QString::fromLatin1(pattern))); };
    Shape s = Shape::Relaxed;
    if (has("\\b(fists?|punch\\w*|box\\w*|clench\\w*|jab\\w*|uppercut\\w*)\\b")) s = Shape::Fist;
    else if (has("\\bpoint\\w*\\b")) s = Shape::Point;
    else if (has("\\b(grab\\w*|hold\\w*|carr(y|ies|ying)|pick\\w*|grip\\w*|lift\\w*|drink\\w*|climb\\w*|pull\\w*)\\b")) s = Shape::Grip;
    else if (has("\\b(wav(e|es|ing)|open\\w*|spread\\w*|high[- ]?five|clap\\w*|jazz hands|stretch\\w*)\\b")) s = Shape::Open;
    HandPair out{s, s};
    const bool left = has("\\bleft (hand|arm)\\b");
    const bool right = has("\\bright (hand|arm)\\b");
    if (left && !right) out.right = Shape::Relaxed;
    if (right && !left) out.left = Shape::Relaxed;
    // Naming a side, or both hands ("both", "hands", "fists", "arms" in the plural), is explicit: keep it as said.
    out.explicitHands = left || right || has("\\b(both|hands|fists|arms|palms)\\b");
    return out;
}

QStringList modeIds()
{
    QStringList ids{QStringLiteral("auto")};
    ids << shapeIds();
    return ids;
}

bool validMode(const QString& mode)
{
    return mode.trimmed().isEmpty() || modeIds().contains(mode.trimmed().toLower());
}

std::vector<Key> keysFor(const QJsonObject& meta, const QString& mode)
{
    const QString md = mode.trimmed().toLower();
    if (!md.isEmpty() && md != QLatin1String("auto")) {
        Shape s;
        if (!shapeFromId(md, &s)) return {};
        return {Key{0.0, HandPair{s, s}}};
    }
    std::vector<Key> keys;
    double t = 0.0;
    for (const QJsonValue& v : meta.value(QStringLiteral("segments")).toArray()) {
        const QJsonObject seg = v.toObject();
        keys.push_back({t, shapeFromPrompt(seg.value(QStringLiteral("text")).toString())});
        t += seg.value(QStringLiteral("duration")).toDouble(0.0);
    }
    return keys;
}

namespace {

// Wrist positions relative to the chest, per frame (forward kinematics of the SOMA motion; root translation
// cancels out). Empty when a joint is missing.
struct WristTrack {
    std::vector<Ogre::Vector3> left, right;
};

WristTrack wristTrack(const Motion& m)
{
    WristTrack w;
    const int lh = m.indexOf("LeftHand");
    const int rh = m.indexOf("RightHand");
    const int chest = m.indexOf("Chest");
    if (lh < 0 || rh < 0 || chest < 0) return w;
    const size_t J = m.names.size();
    std::vector<Ogre::Quaternion> g(J);
    std::vector<Ogre::Vector3> p(J);
    for (int f = 0; f < m.frameCount(); ++f) {
        for (size_t j = 0; j < J; ++j) {
            const int par = m.parents[j];
            const Ogre::Quaternion& l = m.localRot[size_t(f)][j];
            if (par < 0) {
                g[j] = l;
                p[j] = m.restJoints[j];
                continue;
            }
            g[j] = g[size_t(par)] * l;
            p[j] = p[size_t(par)] + g[size_t(par)] * (m.restJoints[j] - m.restJoints[size_t(par)]);
        }
        w.left.push_back(p[size_t(lh)] - p[size_t(chest)]);
        w.right.push_back(p[size_t(rh)] - p[size_t(chest)]);
    }
    return w;
}

struct WristStats {
    double path = 0.0;     ///< metres travelled relative to the chest
    double height = 0.0;   ///< mean height relative to the chest
};

WristStats statsOf(const std::vector<Ogre::Vector3>& t, int from, int to)
{
    WristStats s;
    if (to <= from) return s;
    for (int f = from; f < to; ++f) {
        s.height += t[size_t(f)].y;
        if (f > from) s.path += t[size_t(f)].distance(t[size_t(f - 1)]);
    }
    s.height /= double(to - from);
    return s;
}

} // namespace

int pickActiveHands(const Motion& m, std::vector<Key>* keys, double minRatio)
{
    if (!keys || keys->empty()) return 0;
    const WristTrack w = wristTrack(m);
    if (w.left.empty()) return 0;
    int narrowed = 0;
    const int T = m.frameCount();
    for (size_t k = 0; k < keys->size(); ++k) {
        HandPair& h = (*keys)[k].hands;
        if (h.explicitHands || h.left != h.right || h.left == Shape::Relaxed) continue;
        const int from = std::clamp(int(std::lround((*keys)[k].time * m.fps)), 0, T);
        const int to = k + 1 < keys->size() ? std::clamp(int(std::lround((*keys)[k + 1].time * m.fps)), from, T) : T;
        const WristStats l = statsOf(w.left, from, to);
        const WristStats r = statsOf(w.right, from, to);
        bool leftOnly = false;
        bool rightOnly = false;
        if (l.path >= minRatio * r.path && l.path > 0.05) leftOnly = true;
        else if (r.path >= minRatio * l.path && r.path > 0.05) rightOnly = true;
        else if (l.height > r.height + 0.15) leftOnly = true;
        else if (r.height > l.height + 0.15) rightOnly = true;
        if (leftOnly) h.right = Shape::Relaxed;
        if (rightOnly) h.left = Shape::Relaxed;
        narrowed += (leftOnly || rightOnly) ? 1 : 0;
    }
    return narrowed;
}

int apply(Motion* m, const std::vector<Key>& keys, double blendSeconds, QString* why)
{
    const auto fail = [why](const QString& msg) {
        if (why) *why = msg;
        return 0;
    };
    if (!m || m->frameCount() == 0) return fail(QStringLiteral("no motion"));
    if (keys.empty() || !anyShape(keys)) return fail(QStringLiteral("every hand is relaxed"));
    const Hand left = handOf(*m, "Left");
    const Hand right = handOf(*m, "Right");
    if (left.joints.empty() && right.joints.empty()) return fail(QStringLiteral("the motion has no SOMA finger joints"));
    std::vector<Key> sorted = keys;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
    for (int f = 0; f < m->frameCount(); ++f) {
        const ShapeAt s = shapeAt(sorted, double(f) / double(m->fps), blendSeconds);
        writeHand(m, left, s.from.left, s.to.left, s.weight, f);
        writeHand(m, right, s.from.right, s.to.right, s.weight, f);
    }
    return int(std::max(left.joints.size(), right.joints.size()));
}

} // namespace AIMotion::HandShapes
