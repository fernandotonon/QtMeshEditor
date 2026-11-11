// HandShapes: the prompt -> shape table, the keys from a motion's metadata, and the finger curls on a synthetic
// SOMA-named hand (fingers must curl toward the palm, Relaxed must leave the motion alone).

#include "HandShapes.h"

#include <QJsonArray>
#include <QJsonObject>

#include <gtest/gtest.h>

#include <cmath>

using namespace AIMotion;
using namespace AIMotion::HandShapes;

namespace {

// A right hand at the origin, fingers along +X, palm facing -Y (the thumb base sits on the palm side), at rest.
// `mirrored` = a LEFT hand: the right hand reflected across the z = 0 plane (thumb and pinky swap sides).
Motion syntheticHand(int frames = 40, bool mirrored = false)
{
    Motion m;
    m.fps = 30.0f;
    const std::string side = mirrored ? "Left" : "Right";
    const auto add = [&m, mirrored, &side](std::string name, int parent, Ogre::Vector3 p) {
        if (mirrored) {
            p.z = -p.z;
            name.replace(0, 5, side);   // "Right..." -> "Left..."
        }
        m.names.push_back(name);
        m.parents.push_back(parent);
        m.restJoints.push_back(p);
        return int(m.names.size()) - 1;
    };
    const int hand = add("RightHand", -1, {0, 0, 0});
    const float zs[4] = {0.03f, 0.01f, -0.01f, -0.03f};
    const char* fingers[4] = {"Index", "Middle", "Ring", "Pinky"};
    for (int f = 0; f < 4; ++f) {
        const std::string b = std::string("RightHand") + fingers[f];
        int p = add(b + "1", hand, {0.02f, 0, zs[f]});
        p = add(b + "2", p, {0.08f, 0, zs[f]});
        p = add(b + "3", p, {0.11f, 0, zs[f]});
        p = add(b + "4", p, {0.13f, 0, zs[f]});
        add(b + "End", p, {0.15f, 0, zs[f]});
    }
    int p = add("RightHandThumb1", hand, {0.02f, -0.02f, 0.04f});
    p = add("RightHandThumb2", p, {0.04f, -0.025f, 0.06f});
    p = add("RightHandThumb3", p, {0.06f, -0.03f, 0.075f});
    add("RightHandThumbEnd", p, {0.08f, -0.035f, 0.085f});
    m.localRot.assign(size_t(frames), std::vector<Ogre::Quaternion>(m.names.size(), Ogre::Quaternion::IDENTITY));
    m.rootPos.assign(size_t(frames), Ogre::Vector3::ZERO);
    return m;
}

// Forward kinematics of one frame: global joint positions.
std::vector<Ogre::Vector3> positions(const Motion& m, int frame)
{
    std::vector<Ogre::Quaternion> g(m.names.size());
    std::vector<Ogre::Vector3> p(m.names.size());
    for (size_t j = 0; j < m.names.size(); ++j) {
        const int par = m.parents[j];
        if (par < 0) {
            g[j] = m.localRot[size_t(frame)][j];
            p[j] = m.restJoints[j];
            continue;
        }
        g[j] = g[size_t(par)] * m.localRot[size_t(frame)][j];
        p[j] = p[size_t(par)] + g[size_t(par)] * (m.restJoints[j] - m.restJoints[size_t(par)]);
    }
    return p;
}

QJsonObject metaWith(std::initializer_list<std::pair<const char*, double>> segs)
{
    QJsonArray a;
    for (const auto& [text, d] : segs) a.append(QJsonObject{{"text", QString::fromLatin1(text)}, {"duration", d}});
    return QJsonObject{{"segments", a}};
}

} // namespace

TEST(HandShapes, PromptWordsPickAShape)
{
    EXPECT_EQ(shapeFromPrompt("a boxer throws punches").left, Shape::Fist);
    EXPECT_EQ(shapeFromPrompt("someone grabs a cup and drinks").right, Shape::Grip);
    EXPECT_EQ(shapeFromPrompt("a person points at the sky").left, Shape::Point);
    EXPECT_EQ(shapeFromPrompt("a person waves hello").right, Shape::Open);
    EXPECT_EQ(shapeFromPrompt("a person walks forward").left, Shape::Relaxed);
    const HandPair r = shapeFromPrompt("a person waves with the right hand");
    EXPECT_EQ(r.right, Shape::Open);
    EXPECT_EQ(r.left, Shape::Relaxed) << "a named hand shapes that hand only";
    EXPECT_EQ(shapeFromPrompt("Points with the left arm").right, Shape::Relaxed);
}

TEST(HandShapes, KeysFollowTheSegmentsOrTheChosenShape)
{
    const QJsonObject meta = metaWith({{"walk forward", 2.0}, {"punch", 1.5}, {"wave", 1.0}});
    const auto keys = keysFor(meta, QStringLiteral("auto"));
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_DOUBLE_EQ(keys[1].time, 2.0);
    EXPECT_DOUBLE_EQ(keys[2].time, 3.5);
    EXPECT_EQ(keys[1].hands.left, Shape::Fist);
    EXPECT_EQ(keys[2].hands.right, Shape::Open);
    const auto fixed = keysFor(meta, QStringLiteral("grip"));
    ASSERT_EQ(fixed.size(), 1u);
    EXPECT_EQ(fixed[0].hands.right, Shape::Grip);
    EXPECT_TRUE(keysFor(meta, QStringLiteral("claw")).empty());
    EXPECT_TRUE(validMode(QString()));
    EXPECT_TRUE(validMode(QStringLiteral("Fist")));
    EXPECT_FALSE(validMode(QStringLiteral("claw")));
}

TEST(HandShapes, FistCurlsTheFingersTowardThePalm)
{
    Motion m = syntheticHand();
    const auto rest = positions(m, 0);
    const int tip = m.indexOf("RightHandMiddleEnd");
    ASSERT_GT(apply(&m, {Key{0.0, {Shape::Fist, Shape::Fist}}}), 0);
    const auto fist = positions(m, 10);
    // A fist turns the finger ~250 degrees, so the TIP comes back up beside the palm; the middle knuckle shows the
    // direction: it must drop to the palm side (-Y), not rise to the back of the hand.
    const int knuckle = m.indexOf("RightHandMiddle3");
    EXPECT_LT(fist[size_t(knuckle)].y, rest[size_t(knuckle)].y - 0.02f) << "the finger bends to the palm side (-Y)";
    EXPECT_LT(fist[size_t(tip)].distance(Ogre::Vector3::ZERO), rest[size_t(tip)].distance(Ogre::Vector3::ZERO) - 0.04f);
    const int thumb = m.indexOf("RightHandThumbEnd");
    EXPECT_LT(fist[size_t(thumb)].y, rest[size_t(thumb)].y);
}

TEST(HandShapes, PointKeepsTheIndexStraight)
{
    Motion m = syntheticHand();
    const auto rest = positions(m, 0);
    apply(&m, {Key{0.0, {Shape::Point, Shape::Point}}});
    const auto p = positions(m, 5);
    const int index = m.indexOf("RightHandIndexEnd");
    const int middle = m.indexOf("RightHandMiddleEnd");
    EXPECT_LT(p[size_t(index)].distance(rest[size_t(index)]), 1e-4f);
    EXPECT_GT(p[size_t(middle)].distance(rest[size_t(middle)]), 0.04f);
}

TEST(HandShapes, ChangesBlendAndRelaxedKeepsTheMotion)
{
    Motion m = syntheticHand(60);
    // A recognisable "own" pose on one joint: Relaxed must keep it.
    const int j = m.indexOf("RightHandRing3");
    const Ogre::Quaternion own(Ogre::Degree(7), Ogre::Vector3::UNIT_Y);
    for (auto& f : m.localRot) f[size_t(j)] = own;
    ASSERT_GT(apply(&m, {Key{0.0, {}}, Key{1.0, {Shape::Fist, Shape::Fist}}}, 0.2), 0);
    EXPECT_TRUE(m.localRot[10][size_t(j)].equals(own, Ogre::Radian(1e-4f))) << "relaxed before the change";
    const Ogre::Quaternion full = m.localRot[50][size_t(j)];
    const Ogre::Quaternion mid = m.localRot[33][size_t(j)];   // 1.1 s: half way through the 0.2 s blend
    const Ogre::Quaternion expected = Ogre::Quaternion::Slerp(0.5f, own, full, true);
    EXPECT_TRUE(mid.equals(expected, Ogre::Radian(1e-3f))) << "exactly half way between the relaxed pose and the fist";
}

TEST(HandShapes, NothingToDoIsReported)
{
    Motion m = syntheticHand();
    QString why;
    EXPECT_EQ(apply(&m, {Key{0.0, {}}}, 0.2, &why), 0);
    EXPECT_TRUE(why.contains(QStringLiteral("relaxed")));
    Motion body;
    body.names = {"Hips"};
    body.parents = {-1};
    body.restJoints = {Ogre::Vector3::ZERO};
    body.localRot.assign(3, {Ogre::Quaternion::IDENTITY});
    EXPECT_EQ(apply(&body, {Key{0.0, {Shape::Fist, Shape::Fist}}}, 0.2, &why), 0);
    EXPECT_TRUE(why.contains(QStringLiteral("finger")));
}

namespace {

// Hips -> Chest -> {Left,Right}Arm -> {Left,Right}Hand; each arm swings by its own amplitude (degrees) or is held
// raised by `liftDeg`.
Motion armsBody(double leftSwing, double rightSwing, double leftLift = 0.0, int frames = 60)
{
    Motion m;
    m.fps = 30.0f;
    m.names = {"Hips", "Chest", "LeftArm", "LeftHand", "RightArm", "RightHand"};
    m.parents = {-1, 0, 1, 2, 1, 4};
    m.restJoints = {{0, 1, 0}, {0, 1.4f, 0}, {-0.2f, 1.4f, 0}, {-0.2f, 0.9f, 0}, {0.2f, 1.4f, 0}, {0.2f, 0.9f, 0}};
    for (int f = 0; f < frames; ++f) {
        std::vector<Ogre::Quaternion> r(6, Ogre::Quaternion::IDENTITY);
        const double s = std::sin(f * 0.3);
        r[2] = Ogre::Quaternion(Ogre::Degree(float(leftSwing * s)), Ogre::Vector3::UNIT_X)
               * Ogre::Quaternion(Ogre::Degree(float(leftLift)), Ogre::Vector3::UNIT_Z);
        r[4] = Ogre::Quaternion(Ogre::Degree(float(rightSwing * s)), Ogre::Vector3::UNIT_X);
        m.localRot.push_back(r);
    }
    m.rootPos.assign(size_t(frames), Ogre::Vector3::ZERO);
    return m;
}

} // namespace

TEST(HandShapes, TheMovingHandTakesTheShape)
{
    std::vector<Key> keys{Key{0.0, shapeFromPrompt("a person waves hello")}};
    ASSERT_EQ(keys[0].hands.left, Shape::Open);
    ASSERT_FALSE(keys[0].hands.explicitHands);
    EXPECT_EQ(pickActiveHands(armsBody(2.0, 40.0), &keys), 1);
    EXPECT_EQ(keys[0].hands.right, Shape::Open);
    EXPECT_EQ(keys[0].hands.left, Shape::Relaxed) << "the still left hand stays relaxed";
}

TEST(HandShapes, BothHandsWhenBothMoveOrThePromptSaysSo)
{
    std::vector<Key> boxer{Key{0.0, shapeFromPrompt("a boxer punches")}};
    EXPECT_EQ(pickActiveHands(armsBody(35.0, 40.0), &boxer), 0);
    EXPECT_EQ(boxer[0].hands.left, Shape::Fist);
    EXPECT_EQ(boxer[0].hands.right, Shape::Fist);
    // "fists" (plural) names both hands: kept even though only one arm moves.
    std::vector<Key> said{Key{0.0, shapeFromPrompt("clench both fists")}};
    ASSERT_TRUE(said[0].hands.explicitHands);
    EXPECT_EQ(pickActiveHands(armsBody(0.0, 40.0), &said), 0);
    EXPECT_EQ(said[0].hands.left, Shape::Fist);
}

TEST(HandShapes, ARaisedHandTakesTheShapeWhenNeitherMoves)
{
    // Pointing: the left arm is held up to the side (90 deg), neither swings.
    std::vector<Key> keys{Key{0.0, shapeFromPrompt("a person points at something")}};
    EXPECT_EQ(pickActiveHands(armsBody(0.0, 0.0, -90.0), &keys), 1);
    EXPECT_EQ(keys[0].hands.left, Shape::Point);
    EXPECT_EQ(keys[0].hands.right, Shape::Relaxed);
}

TEST(HandShapes, EachSegmentPicksItsOwnHand)
{
    // Two keys over one take: the right arm moves only in the first second, the left only in the second.
    Motion m = armsBody(0.0, 0.0, 0.0, 60);
    for (int f = 0; f < 60; ++f) {
        const double s = std::sin(f * 0.3) * 40.0;
        auto& r = m.localRot[size_t(f)];
        if (f < 30) r[4] = Ogre::Quaternion(Ogre::Degree(float(s)), Ogre::Vector3::UNIT_X);
        else r[2] = Ogre::Quaternion(Ogre::Degree(float(s)), Ogre::Vector3::UNIT_X);
    }
    std::vector<Key> keys{Key{0.0, shapeFromPrompt("wave")}, Key{1.0, shapeFromPrompt("grab a cup")}};
    EXPECT_EQ(pickActiveHands(m, &keys), 2);
    EXPECT_EQ(keys[0].hands.right, Shape::Open);
    EXPECT_EQ(keys[0].hands.left, Shape::Relaxed);
    EXPECT_EQ(keys[1].hands.left, Shape::Grip);
    EXPECT_EQ(keys[1].hands.right, Shape::Relaxed);
}

TEST(HandShapes, AFistThumbCrossesThePalmTowardTheLittleFinger)
{
    // The thumb must swing ACROSS the palm (toward the pinky side, -Z here) rather than fold straight into the palm
    // like a finger: the into-the-palm bend made the thumb stick out sideways (overbent) on real rigs.
    Motion m = syntheticHand();
    const auto rest = positions(m, 0);
    apply(&m, {Key{0.0, {Shape::Fist, Shape::Fist}}});
    const auto fist = positions(m, 5);
    const int tip = m.indexOf("RightHandThumbEnd");
    EXPECT_LT(fist[size_t(tip)].z, rest[size_t(tip)].z - 0.02f) << "the thumb tip moves toward the little finger";
    EXPECT_LT(fist[size_t(tip)].y, rest[size_t(tip)].y + 0.005f) << "and stays on the palm side";
}

TEST(HandShapes, ALeftHandCurlsTheSameWayAsTheRight)
{
    // The left hand mirrors the right: the fingers still fold to the palm side and the thumb still swings toward
    // the little finger (now on +Z). A fixed-sign axis would curl one of the two hands the wrong way.
    Motion m = syntheticHand(40, true);
    const auto rest = positions(m, 0);
    ASSERT_GT(apply(&m, {Key{0.0, {Shape::Fist, Shape::Fist}}}), 0);
    const auto fist = positions(m, 5);
    const int knuckle = m.indexOf("LeftHandMiddle3");
    const int thumb = m.indexOf("LeftHandThumbEnd");
    ASSERT_GE(knuckle, 0);
    EXPECT_LT(fist[size_t(knuckle)].y, rest[size_t(knuckle)].y - 0.02f) << "fingers fold to the palm side";
    EXPECT_GT(fist[size_t(thumb)].z, rest[size_t(thumb)].z + 0.02f) << "the thumb moves toward the little finger";
}

TEST(HandShapes, AmbiguousWholeBodyVerbsKeepRelaxedHands)
{
    EXPECT_EQ(shapeFromPrompt("a person stretches").left, Shape::Relaxed);
    EXPECT_EQ(shapeFromPrompt("a person pulls away").left, Shape::Relaxed);
    EXPECT_EQ(shapeFromPrompt("a person pulls the rope").left, Shape::Grip);
    EXPECT_EQ(shapeFromPrompt("someone opens the hands").right, Shape::Open);
    EXPECT_EQ(shapeFromPrompt("a person opens the door").right, Shape::Relaxed);
}

TEST(HandShapes, MissingSegmentDurationsAreReported)
{
    QJsonArray segs;
    segs.append(QJsonObject{{"text", "punch"}});   // no duration: the next segment's start is unknown
    segs.append(QJsonObject{{"text", "wave"}, {"duration", 1.0}});
    QString why;
    EXPECT_TRUE(keysFor(QJsonObject{{"segments", segs}}, QStringLiteral("auto"), &why).empty());
    EXPECT_TRUE(why.contains(QStringLiteral("duration")));
    // The LAST segment's duration is never needed.
    QJsonArray last;
    last.append(QJsonObject{{"text", "punch"}, {"duration", 1.0}});
    last.append(QJsonObject{{"text", "wave"}});
    EXPECT_EQ(keysFor(QJsonObject{{"segments", last}}, QStringLiteral("auto")).size(), 2u);
}
