#include <gtest/gtest.h>

#include "MotionInterchange.h"

#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstring>
#include <limits>

using namespace AIMotion;

namespace {

void put16(QByteArray& b, quint16 v) { b.append(char(v & 0xFF)); b.append(char(v >> 8)); }
void put32(QByteArray& b, quint32 v) { for (int i = 0; i < 4; ++i) b.append(char((v >> (8 * i)) & 0xFF)); }

QByteArray npy(const QByteArray& descr, const QByteArray& shape, const QByteArray& payload)
{
    QByteArray hdr = "{'descr': '" + descr + "', 'fortran_order': False, 'shape': " + shape + ", }";
    while ((10 + hdr.size()) % 64 != 0) hdr.append(' ');
    hdr.append('\n');
    QByteArray out("\x93NUMPY", 6);
    out.append(char(1)); out.append(char(0));
    put16(out, quint16(hdr.size()));
    return out + hdr + payload;
}

bool writeNpz(const QString& path, const std::vector<std::pair<QByteArray, QByteArray>>& members)
{
    QByteArray out;
    struct Rec { QByteArray name; quint32 ofs, size; };
    std::vector<Rec> recs;
    for (const auto& m : members) {
        Rec r{m.first + ".npy", quint32(out.size()), quint32(m.second.size())};
        out.append("PK\x03\x04", 4);
        put16(out, 20); put16(out, 0); put16(out, 0); put16(out, 0); put16(out, 0);
        put32(out, 0); put32(out, r.size); put32(out, r.size);
        put16(out, quint16(r.name.size())); put16(out, 0);
        out.append(r.name); out.append(m.second);
        recs.push_back(r);
    }
    const quint32 cd = quint32(out.size());
    for (const auto& r : recs) {
        out.append("PK\x01\x02", 4);
        put16(out, 20); put16(out, 20); put16(out, 0); put16(out, 0); put16(out, 0); put16(out, 0);
        put32(out, 0); put32(out, r.size); put32(out, r.size);
        put16(out, quint16(r.name.size())); put16(out, 0); put16(out, 0); put16(out, 0); put16(out, 0);
        put32(out, 0); put32(out, r.ofs);
        out.append(r.name);
    }
    const quint32 cdSize = quint32(out.size()) - cd;
    out.append("PK\x05\x06", 4);
    put16(out, 0); put16(out, 0); put16(out, quint16(recs.size())); put16(out, quint16(recs.size()));
    put32(out, cdSize); put32(out, cd); put16(out, 0);
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(out) == out.size();
}

QByteArray floats(const std::vector<float>& v)
{
    QByteArray b(int(v.size() * 4), '\0');
    std::memcpy(b.data(), v.data(), v.size() * 4);
    return b;
}

QByteArray names(const std::vector<QByteArray>& n, int width)
{
    QByteArray b;
    for (QByteArray s : n) { s.append(QByteArray(width - s.size(), '\0')); b.append(s); }
    return b;
}

// A 3-joint chain (Hips -> Spine1 -> Head), T frames: Spine1 turns about Y by 90 degrees * t/(T-1), the root walks along +Z.
struct Chain {
    int T = 4;
    std::vector<float> rots, root, rest{0, 1, 0, 0, 1.1f, 0, 0, 1.6f, 0};
    std::vector<float> parents{-1, 0, 1};
    float version = 1.0f;
    Chain()
    {
        for (int t = 0; t < T; ++t) {
            const float a = float(M_PI / 2) * float(t) / float(T - 1);
            const float c = std::cos(a), s = std::sin(a);
            const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            const float Ry[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
            rots.insert(rots.end(), I, I + 9);
            rots.insert(rots.end(), Ry, Ry + 9);
            rots.insert(rots.end(), I, I + 9);
            root.insert(root.end(), {0.0f, 1.0f, 0.5f * float(t)});
        }
    }
    bool write(const QString& path) const
    {
        const QByteArray shapeT = "(" + QByteArray::number(T);
        return writeNpz(path, {
            {"format_version", npy("<f4", "(1,)", floats({version}))},
            {"fps", npy("<f4", "(1,)", floats({30.0f}))},
            {"joint_names", npy("|S6", "(3,)", names({"Hips", "Spine1", "Head"}, 6))},
            {"joint_parents", npy("<f4", "(3,)", floats(parents))},
            {"rest_joints", npy("<f4", "(3, 3)", floats(rest))},
            {"local_rot_mats", npy("<f4", shapeT + ", 3, 3, 3)", floats(rots))},
            {"root_positions", npy("<f4", shapeT + ", 3)", floats(root))},
        });
    }
};

} // namespace

TEST(MotionInterchange, ReadsAChainAndConvertsRotations)
{
    QTemporaryDir dir;
    const QString p = dir.filePath(QStringLiteral("m.npz"));
    ASSERT_TRUE(Chain().write(p));
    Motion m;
    QString err;
    ASSERT_TRUE(loadInterchange(p, &m, &err)) << err.toStdString();
    EXPECT_EQ(m.jointCount(), 3);
    EXPECT_EQ(m.frameCount(), 4);
    EXPECT_EQ(m.parents, (std::vector<int>{-1, 0, 1}));
    EXPECT_NEAR(m.duration(), 0.1f, 1e-5f);
    // Last frame: 90 degrees about +Y.
    const Ogre::Quaternion want(Ogre::Radian(float(M_PI / 2)), Ogre::Vector3::UNIT_Y);
    EXPECT_TRUE(m.localRot[3][1].equals(want, Ogre::Radian(1e-3f)));
    EXPECT_NEAR(m.rootPos[3].z, 1.5f, 1e-5f);
}

TEST(MotionInterchange, RigDescUsesParentRelativeOffsets)
{
    QTemporaryDir dir;
    const QString p = dir.filePath(QStringLiteral("m.npz"));
    ASSERT_TRUE(Chain().write(p));
    Motion m;
    ASSERT_TRUE(loadInterchange(p, &m));
    const Retarget::RigDesc rig = rigDesc(m);
    EXPECT_TRUE(rig.bindPos[0].positionEquals(Ogre::Vector3(0, 1, 0)));
    EXPECT_TRUE(rig.bindPos[2].positionEquals(Ogre::Vector3(0, 0.5f, 0)));
    const auto poses = localPoses(m);
    ASSERT_EQ(poses.size(), 4u);
    EXPECT_NEAR(poses[2].pos[0].z, 1.0f, 1e-5f) << "the root carries the frame's world position";
    EXPECT_TRUE(poses[2].pos[1].positionEquals(rig.bindPos[1])) << "other joints keep the bind offsets";
}

TEST(MotionInterchange, RigDescStandsAHipsRelativeRestOnTheFloor)
{
    // Kimodo's rest joints are hips-relative (hips at the origin, feet below) while root_positions are in floor space.
    // The rest root must be lifted onto the same floor, else the retarget lifts every frame by the hip height.
    QTemporaryDir dir;
    Chain c;
    c.rest = {0, 0, 0, 0, -0.9f, 0, 0, 0.6f, 0};   // a joint 0.9 m below the hips
    const QString p = dir.filePath(QStringLiteral("r.npz"));
    ASSERT_TRUE(c.write(p));
    Motion m;
    ASSERT_TRUE(loadInterchange(p, &m));
    const Retarget::RigDesc rig = rigDesc(m);
    EXPECT_TRUE(rig.bindPos[0].positionEquals(Ogre::Vector3(0, 0.9f, 0))) << "rest hips stand 0.9 m above the floor";
    EXPECT_TRUE(rig.bindPos[1].positionEquals(Ogre::Vector3(0, -0.9f, 0))) << "child offsets are unchanged";
}

TEST(MotionInterchange, RefusesAnUnknownVersionAndBadParents)
{
    QTemporaryDir dir;
    Chain c;
    c.version = 2.0f;
    const QString p = dir.filePath(QStringLiteral("v.npz"));
    ASSERT_TRUE(c.write(p));
    Motion m;
    QString err;
    EXPECT_FALSE(loadInterchange(p, &m, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("version")));

    Chain bad;
    bad.parents = {-1, 2, 0};   // child before parent
    const QString q = dir.filePath(QStringLiteral("p.npz"));
    ASSERT_TRUE(bad.write(q));
    EXPECT_FALSE(loadInterchange(q, &m, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("parent")));
}

TEST(MotionInterchange, RefusesAReflectionMatrix)
{
    QTemporaryDir dir;
    Chain c;
    c.rots[0] = -1.0f;   // frame 0, Hips: det = -1
    const QString p = dir.filePath(QStringLiteral("r.npz"));
    ASSERT_TRUE(c.write(p));
    Motion m;
    QString err;
    EXPECT_FALSE(loadInterchange(p, &m, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("not a rotation")));
}

TEST(MotionInterchange, ResampleKeepsDurationAndEndpoints)
{
    QTemporaryDir dir;
    const QString p = dir.filePath(QStringLiteral("m.npz"));
    ASSERT_TRUE(Chain().write(p));
    Motion m;
    ASSERT_TRUE(loadInterchange(p, &m));
    const Motion r = resample(m, 60.0f);
    EXPECT_NEAR(r.duration(), m.duration(), 1e-4f);
    EXPECT_EQ(r.frameCount(), 7);
    EXPECT_TRUE(r.localRot.back()[1].equals(m.localRot.back()[1], Ogre::Radian(1e-4f)));
    EXPECT_NEAR(r.rootPos[1].z, 0.25f, 1e-4f) << "halfway between frames 0 and 1";
    // Halfway rotation is the slerp midpoint: 15 degrees.
    const Ogre::Quaternion half(Ogre::Radian(float(M_PI / 12)), Ogre::Vector3::UNIT_Y);
    EXPECT_TRUE(r.localRot[1][1].equals(half, Ogre::Radian(1e-3f)));
}

TEST(MotionInterchange, RefusesNonFiniteValues)
{
    QTemporaryDir dir;
    Motion m;
    QString err;
    Chain root;
    root.root[4] = std::numeric_limits<float>::quiet_NaN();   // frame 1, y
    const QString p = dir.filePath(QStringLiteral("root.npz"));
    ASSERT_TRUE(root.write(p));
    EXPECT_FALSE(loadInterchange(p, &m, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("root_positions")));

    Chain parents;
    parents.parents[1] = std::numeric_limits<float>::infinity();
    const QString q = dir.filePath(QStringLiteral("parents.npz"));
    ASSERT_TRUE(parents.write(q));
    EXPECT_FALSE(loadInterchange(q, &m, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("joint_parents")));
}
