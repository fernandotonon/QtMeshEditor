// #525 — constraint maths + JSON (pure data).

#include <gtest/gtest.h>

#include "AnimConstraints.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <OgreMatrix3.h>

using namespace AnimCon;

namespace {
// Quaternion::equals takes acos of a float dot product and reports ~5e-4 rad
// between IDENTICAL rotations; compare |dot| instead (q and -q are the same).
bool sameRotation(const Ogre::Quaternion& a, const Ogre::Quaternion& b, float tol = 1e-5f)
{
    return std::abs(std::abs(a.Dot(b)) - 1.0f) < tol;
}
} // namespace

TEST(AnimConRef, ParsesNodesAndBones)
{
    Ref r;
    ASSERT_TRUE(parseRef(QStringLiteral("node:Target"), &r));
    EXPECT_EQ(r.object, QStringLiteral("Target"));
    EXPECT_FALSE(r.isBone());
    ASSERT_TRUE(parseRef(QStringLiteral("bone:Hero/mixamorig:LeftHand"), &r));
    EXPECT_EQ(r.object, QStringLiteral("Hero"));
    EXPECT_EQ(r.bone, QStringLiteral("mixamorig:LeftHand")) << "bone names may contain ':'";
    EXPECT_EQ(formatRef(r), QStringLiteral("bone:Hero/mixamorig:LeftHand"));
    EXPECT_TRUE(parseRef(QString(), &r));
    EXPECT_TRUE(r.isEmpty());
    EXPECT_FALSE(parseRef(QStringLiteral("light:Key"), &r));
    EXPECT_FALSE(parseRef(QStringLiteral("bone:Hero"), &r));
    Type t;
    EXPECT_TRUE(typeFromId(QStringLiteral("aim"), &t));
    EXPECT_EQ(t, Type::LookAt);
    EXPECT_TRUE(typeFromId(QStringLiteral("child_of"), &t));
    EXPECT_EQ(t, Type::ParentOf);
    EXPECT_FALSE(nodeCanOwn(Type::IK));
    EXPECT_FALSE(needsTarget(Type::LimitRotation));
}

TEST(AnimConAim, PointsTheAimAxisAtTheTargetAndKeepsUpRight)
{
    const Ogre::Vector3 from(1, 2, 3);
    for (const Ogre::Vector3 to : {Ogre::Vector3(5, 2, 3), Ogre::Vector3(1, 2, -4), Ogre::Vector3(-3, 6, 8)}) {
        for (Axis aim : {Axis::Z, Axis::NegZ, Axis::X, Axis::Y}) {
            const Axis up = aim == Axis::Y ? Axis::Z : Axis::Y;
            const Ogre::Quaternion q = aimRotation(from, to, aim, up, Ogre::Vector3::UNIT_Y, Ogre::Quaternion::IDENTITY);
            const Ogre::Vector3 pointed = q * axisVector(aim);
            const Ogre::Vector3 want = (to - from).normalisedCopy();
            EXPECT_LT((pointed - want).length(), 1e-4f) << axisId(aim).toStdString();
            EXPECT_NEAR(q.Norm(), 1.0f, 1e-4f);
        }
    }
    // Up stays as close to world up as the aim allows.
    const Ogre::Quaternion q = aimRotation(Ogre::Vector3::ZERO, Ogre::Vector3(0, 0, 5), Axis::Z, Axis::Y,
                                           Ogre::Vector3::UNIT_Y, Ogre::Quaternion::IDENTITY);
    EXPECT_GT((q * Ogre::Vector3::UNIT_Y).y, 0.999f);
    // Coincident points: keep the current rotation.
    const Ogre::Quaternion cur(Ogre::Degree(30), Ogre::Vector3::UNIT_Y);
    EXPECT_TRUE(sameRotation(aimRotation(from, from, Axis::Z, Axis::Y, Ogre::Vector3::UNIT_Y, cur), cur));
}

TEST(AnimConIk, ReachesAReachableTargetWithTheExactBoneLengths)
{
    const Ogre::Vector3 A(0, 0, 0), B(0, -1, 0.1f), C(0, -2, 0);
    for (const Ogre::Vector3 T : {Ogre::Vector3(0.8f, -1.2f, 0.3f), Ogre::Vector3(1.2f, 0.4f, -0.5f), Ogre::Vector3(0, -0.5f, 0.6f)}) {
        const IkSolution s = solveTwoBone(A, B, C, T, Ogre::Vector3::ZERO, false);
        ASSERT_TRUE(s.valid);
        EXPECT_TRUE(s.reached);
        EXPECT_LT((s.newEnd - T).length(), 1e-3f) << "end effector on the target";
        EXPECT_NEAR((s.newMid - A).length(), (B - A).length(), 1e-4f) << "upper bone length kept";
        EXPECT_NEAR((s.newEnd - s.newMid).length(), (C - B).length(), 1e-4f) << "lower bone length kept";
        // Applying the deltas to the bones' directions reproduces the solved
        // joint positions (the manager does exactly this with orientations).
        const Ogre::Vector3 mid = A + s.rootDelta * (B - A);
        const Ogre::Vector3 end = mid + s.midDelta * (s.rootDelta * (C - B));
        EXPECT_LT((mid - s.newMid).length(), 1e-3f);
        EXPECT_LT((end - T).length(), 1e-3f);
    }
}

TEST(AnimConIk, OutOfReachPointsStraightAndPoleChoosesTheBend)
{
    const Ogre::Vector3 A(0, 0, 0), B(0, -1, 0.1f), C(0, -2, 0);
    const IkSolution far = solveTwoBone(A, B, C, Ogre::Vector3(10, 0, 0), Ogre::Vector3::ZERO, false);
    ASSERT_TRUE(far.valid);
    EXPECT_FALSE(far.reached);
    EXPECT_GT((far.newEnd - A).normalisedCopy().x, 0.999f) << "fully extended toward the target";

    const Ogre::Vector3 T(0, -1.5f, 0);
    const IkSolution poleFront = solveTwoBone(A, B, C, T, Ogre::Vector3(0, 0, 5), true);
    const IkSolution poleBack = solveTwoBone(A, B, C, T, Ogre::Vector3(0, 0, -5), true);
    EXPECT_GT(poleFront.newMid.z, 0.1f) << "the elbow bends toward the pole";
    EXPECT_LT(poleBack.newMid.z, -0.1f);
    EXPECT_FALSE(solveTwoBone(A, A, C, T, Ogre::Vector3::ZERO, false).valid) << "zero-length bone";
}

TEST(AnimConLimit, ClampsOnlyTheEnabledAxes)
{
    Constraint c;
    c.type = Type::LimitRotation;
    c.minDeg = Ogre::Vector3(0, -10, -90);
    c.maxDeg = Ogre::Vector3(120, 10, 90);
    c.limitZ = false;
    Ogre::Matrix3 m;
    m.FromEulerAnglesXYZ(Ogre::Degree(-40), Ogre::Degree(30), Ogre::Degree(150));
    const Ogre::Quaternion out = limitRotation(Ogre::Quaternion(m), c);
    out.ToRotationMatrix(m);
    Ogre::Radian x, y, z;
    m.ToEulerAnglesXYZ(x, y, z);
    EXPECT_NEAR(x.valueDegrees(), 0.0f, 0.05f) << "a knee cannot bend backwards past 0";
    EXPECT_NEAR(y.valueDegrees(), 10.0f, 0.05f);
    EXPECT_NEAR(z.valueDegrees(), 150.0f, 0.05f) << "Z not limited";
}

TEST(AnimConMaths, ComposeInverseAndBlend)
{
    Transform t;
    t.position = Ogre::Vector3(1, 2, 3);
    t.rotation = Ogre::Quaternion(Ogre::Degree(40), Ogre::Vector3(1, 1, 0).normalisedCopy());
    t.scale = Ogre::Vector3(2, 2, 2);
    const Transform id = compose(t, inverse(t));
    EXPECT_LT(id.position.length(), 1e-4f);
    EXPECT_TRUE(sameRotation(id.rotation, Ogre::Quaternion::IDENTITY));
    EXPECT_LT((id.scale - Ogre::Vector3::UNIT_SCALE).length(), 1e-5f);

    EXPECT_LT((blend(Ogre::Vector3::ZERO, Ogre::Vector3(4, 0, 0), 0.25) - Ogre::Vector3(1, 0, 0)).length(), 1e-6f);
    const Ogre::Quaternion q0 = Ogre::Quaternion::IDENTITY, q1(Ogre::Degree(90), Ogre::Vector3::UNIT_Y);
    Ogre::Radian ang;
    Ogre::Vector3 ax;
    blend(q0, q1, 0.5).ToAngleAxis(ang, ax);
    EXPECT_NEAR(ang.valueDegrees(), 45.0f, 0.01f);
    EXPECT_TRUE(sameRotation(blend(q0, q1, 2.0), q1)) << "influence clamps to 1";
}

TEST(AnimConJson, RoundTripsEveryType)
{
    std::vector<Constraint> cs;
    auto mk = [](const char* id, Type t, const char* target) {
        Constraint c;
        c.id = id;
        c.type = t;
        parseRef(QStringLiteral("bone:Hero/Hand"), &c.owner);
        if (target) parseRef(QString::fromLatin1(target), &c.target);
        return c;
    };
    Constraint look = mk("a", Type::LookAt, "node:Ball"); look.aimAxis = Axis::NegZ; look.upAxis = Axis::X; look.influence = 0.4; cs.push_back(look);
    Constraint ik = mk("b", Type::IK, "node:Goal"); parseRef(QStringLiteral("node:Knee"), &ik.pole); ik.enabled = false; cs.push_back(ik);
    Constraint par = mk("c", Type::ParentOf, "bone:Prop/Grip");
    par.hasOffset = true; par.offset.position = Ogre::Vector3(1, 2, 3); par.offset.rotation = Ogre::Quaternion(Ogre::Degree(30), Ogre::Vector3::UNIT_Z);
    cs.push_back(par);
    Constraint cp = mk("d", Type::CopyPosition, "node:Ball"); cp.useY = false; cs.push_back(cp);
    Constraint lim = mk("e", Type::LimitRotation, nullptr); lim.minDeg = Ogre::Vector3(0, -5, -6); lim.limitY = false; cs.push_back(lim);

    std::vector<Constraint> back;
    QString err;
    ASSERT_TRUE(fromDocument(QJsonDocument::fromJson(QJsonDocument(toDocument(cs)).toJson()).object(), &back, &err))
        << err.toStdString();
    ASSERT_EQ(back.size(), cs.size());
    EXPECT_EQ(back[0].aimAxis, Axis::NegZ);
    EXPECT_EQ(back[0].upAxis, Axis::X);
    EXPECT_DOUBLE_EQ(back[0].influence, 0.4);
    EXPECT_EQ(back[1].pole.object, QStringLiteral("Knee"));
    EXPECT_FALSE(back[1].enabled);
    EXPECT_TRUE(back[2].hasOffset);
    EXPECT_LT((back[2].offset.position - Ogre::Vector3(1, 2, 3)).length(), 1e-6f);
    EXPECT_EQ(back[2].target.bone, QStringLiteral("Grip"));
    EXPECT_FALSE(back[3].useY);
    EXPECT_FALSE(back[4].limitY);
    EXPECT_FLOAT_EQ(back[4].minDeg.z, -6.0f);
}

TEST(AnimConJson, RejectsBadInput)
{
    std::vector<Constraint> out;
    QString err;
    EXPECT_FALSE(fromDocument(QJsonObject{{"schema", "nope"}}, &out, &err));
    QJsonObject noTarget{{"id", "x"}, {"type", "look-at"}, {"owner", "node:A"}};
    EXPECT_FALSE(fromDocument(QJsonObject{{"schema", schemaId()}, {"constraints", QJsonArray{noTarget}}}, &out, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("needs a target")));
    QJsonObject ok{{"id", "x"}, {"type", "limit-rotation"}, {"owner", "node:A"}};
    EXPECT_FALSE(fromDocument(QJsonObject{{"schema", schemaId()}, {"constraints", QJsonArray{ok, ok}}}, &out, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("duplicate")));

    Constraint c;
    EXPECT_TRUE(applyParam(&c, "influence", "3", &err));
    EXPECT_DOUBLE_EQ(c.influence, 1.0);
    EXPECT_TRUE(applyParam(&c, "aim", "-x", &err));
    EXPECT_EQ(c.aimAxis, Axis::NegX);
    EXPECT_FALSE(applyParam(&c, "aim", "w", &err));
    EXPECT_FALSE(applyParam(&c, "min_x", "nan", &err));
    EXPECT_FALSE(applyParam(&c, "wobble", "1", &err));
    EXPECT_TRUE(applyParam(&c, "target", "bone:Hero/Head", &err));
    EXPECT_EQ(c.target.bone, QStringLiteral("Head"));
}
