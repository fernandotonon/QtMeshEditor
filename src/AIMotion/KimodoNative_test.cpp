// Native Kimodo runtime: interchange writer, runtime resolution, hosted-file naming, and the vendored
// MotionCorrection entry point. End-to-end parity with Kimodo's PyTorch output needs the 9 GB of models, so it is
// a developer check rather than a CI test: scripts/export-kimodo-onnx.py --verify (numpy replay), then
// verify_kimodo_native.write_fixture + QTMESH_KIMODO_NATIVE_FIXTURE=<npz> to run THIS C++ on the same text
// features and noise (measured: max rotation 2e-4, root 0.003 cm, identical foot contacts).

#include "KimodoCorrection.h"
#include "KimodoMath.h"
#include "KimodoNative.h"
#include "KimodoPredictor.h"
#include "MotionInterchange.h"

#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <cmath>

using namespace AIMotion;

TEST(KimodoNative, InterchangeRoundTrip)
{
    QTemporaryDir dir;
    InterchangeArrays a;
    a.names = {"Hips", "Spine1", "Head"};
    a.parents = {-1, 0, 1};
    a.restJoints = {0, 0, 0, 0, 0.2f, 0, 0, 0.6f, 0};
    a.fps = 30.0f;
    const int T = 3;
    for (int t = 0; t < T; ++t) {
        for (int j = 0; j < 3; ++j) {
            const float c = std::cos(0.1f * float(t)), s = std::sin(0.1f * float(t));
            const float m[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
            a.localRotMats.insert(a.localRotMats.end(), m, m + 9);
        }
        a.rootPositions.insert(a.rootPositions.end(), {0.0f, 0.9f, 0.5f * float(t)});
        a.footContacts.insert(a.footContacts.end(), {1, 0, 0, 0, 1, 1});
    }
    a.contactChannels = 6;
    const QString path = dir.filePath(QStringLiteral("m.npz"));
    QString err;
    ASSERT_TRUE(saveInterchange(path, a, &err)) << err.toStdString();
    Motion m;
    ASSERT_TRUE(loadInterchange(path, &m, &err)) << err.toStdString();
    EXPECT_EQ(m.jointCount(), 3);
    EXPECT_EQ(m.frameCount(), T);
    EXPECT_EQ(m.names[2], "Head");
    EXPECT_EQ(m.parents, (std::vector<int>{-1, 0, 1}));
    EXPECT_NEAR(m.rootPos[2].z, 1.0f, 1e-6);
    ASSERT_EQ(m.contacts.size(), size_t(T));
    EXPECT_EQ(m.contacts[0].size(), 6u);
    const Ogre::Quaternion want(Ogre::Radian(0.2f), Ogre::Vector3::UNIT_Y);
    EXPECT_TRUE(m.localRot[2][1].equals(want, Ogre::Radian(1e-4f)));
    EXPECT_FALSE(QFile::exists(path + QStringLiteral(".part")));
}

TEST(KimodoNative, InterchangeRejectsMismatchedArrays)
{
    QTemporaryDir dir;
    InterchangeArrays a;
    a.names = {"Hips"};
    a.parents = {-1};
    a.restJoints = {0, 0};   // wrong size
    QString err;
    EXPECT_FALSE(saveInterchange(dir.filePath(QStringLiteral("x.npz")), a, &err));
    EXPECT_FALSE(err.isEmpty());
}

TEST(KimodoNative, HostedTextEncoderCarriesTheLlama3Name)
{
    bool sawEncoder = false, sawLicence = false;
    for (const auto& f : KimodoNative::modelFiles()) {
        EXPECT_TRUE(f.url.startsWith(QStringLiteral("https://"))) << f.url.toStdString();
        if (f.name.endsWith(QStringLiteral(".gguf"))) {
            sawEncoder = true;
            // Meta Llama 3 Community License 1.b.i: a derivative model's name starts with "Llama 3".
            EXPECT_TRUE(f.name.startsWith(QStringLiteral("Llama-3"))) << f.name.toStdString();
        }
        if (f.name == QLatin1String("LICENSE-Llama-3.txt")) sawLicence = true;
    }
    EXPECT_TRUE(sawEncoder);
    EXPECT_TRUE(sawLicence);
}

TEST(KimodoNative, RuntimeResolution)
{
    qputenv("QTMESH_KIMODO_ENV", "/nonexistent/kimodo_runtime");
    KimodoPredictor::Options o;
    o.segments = {{QStringLiteral("walk"), 2.0}};
    o.runtime = QStringLiteral("python");
    QString why;
    EXPECT_TRUE(KimodoPredictor::resolveRuntime(o, &why).isEmpty());
    EXPECT_FALSE(why.isEmpty());
    o.runtime = QStringLiteral("native");
    o.constraintsPath = QStringLiteral("c.json");
    why.clear();
    EXPECT_TRUE(KimodoPredictor::resolveRuntime(o, &why).isEmpty()) << "constraints are Python-only";
    EXPECT_TRUE(why.contains(QStringLiteral("Python")));
    o.runtime = QStringLiteral("bogus");
    o.constraintsPath.clear();
    const auto r = KimodoPredictor::predict(o);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("runtime must be")));
    qunsetenv("QTMESH_KIMODO_ENV");
}

namespace {

// Minimal rig: hips -> spine -> two hands; hips -> two legs (thigh, shin, foot, toe).
KimodoCorrection::Rig legRig()
{
    KimodoCorrection::Rig r;
    // 0 Hips, 1 Spine, 2 LHand, 3 RHand, 4 LThigh, 5 LShin, 6 LFoot, 7 LToe, 8 RThigh, 9 RShin, 10 RFoot, 11 RToe
    r.parents = {-1, 0, 1, 1, 0, 4, 5, 6, 0, 8, 9, 10};
    r.translations = {{0, 0.92f, 0}, {0, 0.3f, 0}, {0.6f, 0.2f, 0}, {-0.6f, 0.2f, 0},
                      {0.1f, -0.05f, 0}, {0, -0.42f, 0}, {0, -0.4f, 0}, {0, -0.05f, 0.12f},
                      {-0.1f, -0.05f, 0}, {0, -0.42f, 0}, {0, -0.4f, 0}, {0, -0.05f, 0.12f}};
    r.rotationsXyzw.assign(12, {0, 0, 0, 1});
    r.leftHand = 2;
    r.rightHand = 3;
    r.leftFoot = 6;
    r.rightFoot = 10;
    return r;
}

} // namespace

TEST(KimodoNative, CorrectionWithoutContactsKeepsTheMotion)
{
    // No contacts, no constraints: nothing to correct, so the motion must come back (numerically) unchanged.
    // The real behaviour (feet pinned, constraints enforced) is checked end to end against Kimodo's PyTorch
    // generation by scripts/verify_kimodo_native.py and the native parity run (docs: ai/kimodo/README.md).
    const KimodoCorrection::Rig rig = legRig();
    const int T = 20, J = int(rig.parents.size());
    std::vector<float> root, rot(size_t(T) * J * 4, 0.0f);
    for (int t = 0; t < T; ++t) root.insert(root.end(), {0.01f * float(t), 0.92f, 0.02f * float(t)});
    for (int k = 0; k < T * J; ++k) {
        const float ang = 0.01f * float(k % 7);
        rot[size_t(k) * 4] = std::cos(ang);
        rot[size_t(k) * 4 + 1] = std::sin(ang);
    }
    const std::vector<float> root0 = root, rot0 = rot;
    std::string err;
    ASSERT_TRUE(KimodoCorrection::correctMotion(root, rot, std::vector<float>(root.size(), 0.0f), rot0, KimodoCorrection::Masks{},
                                                std::vector<float>(size_t(T) * 4, 0.0f), rig, 0.5f, 0.04f, &err))
        << err;
    for (size_t k = 0; k < root.size(); ++k) EXPECT_NEAR(root[k], root0[k], 1e-3) << "root " << k;
    for (size_t k = 0; k < rot.size(); k += 4) {
        const float dot = rot[k] * rot0[k] + rot[k + 1] * rot0[k + 1] + rot[k + 2] * rot0[k + 2] + rot[k + 3] * rot0[k + 3];
        EXPECT_GT(std::fabs(dot), 0.9999f) << "joint quaternion " << k / 4;
    }
}

TEST(KimodoNative, CorrectionRejectsBadSizes)
{
    const KimodoCorrection::Rig rig = legRig();
    std::vector<float> root(9, 0.0f), rot(10, 0.0f);
    std::string err;
    EXPECT_FALSE(KimodoCorrection::correctMotion(root, rot, root, rot, {}, std::vector<float>(12, 0.0f), rig, 0.5f, 0.04f, &err));
    EXPECT_FALSE(err.empty());
}
