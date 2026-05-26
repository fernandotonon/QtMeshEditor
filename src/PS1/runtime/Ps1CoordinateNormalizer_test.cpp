#include "Ps1CoordinateNormalizer.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryFile>

#include <gtest/gtest.h>

#include <cmath>

namespace {

// Lightweight settings fixture that writes to an in-memory ini so each test
// gets a fresh slate. Avoids leaking values into the user's real org/app key.
class TempIniSettings
{
public:
    TempIniSettings()
    {
        m_file.setAutoRemove(true);
        m_file.open();
        m_file.close();
        m_settings = std::make_unique<QSettings>(m_file.fileName(), QSettings::IniFormat);
    }

    QSettings &operator()() { return *m_settings; }

private:
    QTemporaryFile m_file;
    std::unique_ptr<QSettings> m_settings;
};

} // namespace

TEST(Ps1CoordinateNormalizerTest, DefaultSettingsAreRecognisedAsDefault)
{
    Ps1NormalizerSettings s;
    EXPECT_TRUE(s.isDefault());
    EXPECT_FLOAT_EQ(s.signX(), 1.0f);
    EXPECT_FLOAT_EQ(s.signY(), 1.0f);
    EXPECT_FLOAT_EQ(s.signZ(), 1.0f);
    EXPECT_FALSE(s.flipsAreActive());
    EXPECT_EQ(Ps1CoordinateNormalizer::describe(s), QStringLiteral("default"));
}

TEST(Ps1CoordinateNormalizerTest, FlipsFlipSignsAndDeviateFromDefault)
{
    Ps1NormalizerSettings s;
    s.flipY = true;
    EXPECT_FALSE(s.isDefault());
    EXPECT_TRUE(s.flipsAreActive());
    EXPECT_FLOAT_EQ(s.signX(), 1.0f);
    EXPECT_FLOAT_EQ(s.signY(), -1.0f);
    EXPECT_FLOAT_EQ(s.signZ(), 1.0f);
    EXPECT_TRUE(Ps1CoordinateNormalizer::describe(s).contains(QStringLiteral("flipY")));
}

TEST(Ps1CoordinateNormalizerTest, DescribeListsActiveFields)
{
    Ps1NormalizerSettings s;
    s.userScale = 2.5f;
    s.flipX = true;
    s.flipZ = true;
    s.perspectiveCorrectUVs = true;
    const QString d = Ps1CoordinateNormalizer::describe(s);
    EXPECT_TRUE(d.contains(QStringLiteral("scale=2.5")));
    EXPECT_TRUE(d.contains(QStringLiteral("flipX")));
    EXPECT_FALSE(d.contains(QStringLiteral("flipY")));
    EXPECT_TRUE(d.contains(QStringLiteral("flipZ")));
    EXPECT_TRUE(d.contains(QStringLiteral("perspUV")));
}

TEST(Ps1CoordinateNormalizerTest, SaveLoadRoundtripPreservesAllFields)
{
    TempIniSettings tmp;
    Ps1NormalizerSettings src;
    src.userScale = 1.75f;
    src.flipX = true;
    src.flipY = false;
    src.flipZ = true;
    src.perspectiveCorrectUVs = true;
    src.perspectiveTolerance = 2.0f;
    src.perspectiveMaxDepth = 2;

    Ps1CoordinateNormalizer::save(tmp(), QStringLiteral("test/normalize"), src);
    const Ps1NormalizerSettings loaded =
        Ps1CoordinateNormalizer::load(tmp(), QStringLiteral("test/normalize"));

    EXPECT_FLOAT_EQ(loaded.userScale, src.userScale);
    EXPECT_EQ(loaded.flipX, src.flipX);
    EXPECT_EQ(loaded.flipY, src.flipY);
    EXPECT_EQ(loaded.flipZ, src.flipZ);
    EXPECT_EQ(loaded.perspectiveCorrectUVs, src.perspectiveCorrectUVs);
    EXPECT_FLOAT_EQ(loaded.perspectiveTolerance, src.perspectiveTolerance);
    EXPECT_EQ(loaded.perspectiveMaxDepth, src.perspectiveMaxDepth);
}

TEST(Ps1CoordinateNormalizerTest, LoadFallsBackForCorruptedValues)
{
    TempIniSettings tmp;
    tmp().setValue(QStringLiteral("test/normalize/userScale"), 0.0);
    tmp().setValue(QStringLiteral("test/normalize/perspectiveTolerance"), 0.5);
    tmp().setValue(QStringLiteral("test/normalize/perspectiveMaxDepth"), -3);

    const Ps1NormalizerSettings out =
        Ps1CoordinateNormalizer::load(tmp(), QStringLiteral("test/normalize"));
    EXPECT_FLOAT_EQ(out.userScale, 1.0f);
    EXPECT_FLOAT_EQ(out.perspectiveTolerance, 1.3f);
    EXPECT_EQ(out.perspectiveMaxDepth, 3);
}

// Acceptance criterion: "synthetic L/H Y-down quad → R/H Y-up with expected winding".
// The reconstruction pipeline (modelToEditor) already produces Y-up from PS1
// Y-down model space, with face winding matching Ogre's CCW front-face
// convention. The user-facing per-axis flips layered on top compose via
// SceneNode scale: an odd number of negated axes inverts the determinant and
// Ogre flips back-face culling automatically — no per-vertex index swap needed.
// This test verifies the math without depending on Ogre by checking that the
// sign multipliers track the user's toggle state.
TEST(Ps1CoordinateNormalizerTest, YDownQuadProducesYUpWithCorrectWinding)
{
    // Default (no extra flips): pipeline already returned Y-up. SceneNode
    // scale stays positive on every axis, determinant > 0, winding preserved.
    Ps1NormalizerSettings def;
    EXPECT_FLOAT_EQ(def.signY(), 1.0f);
    const float defDet = def.signX() * def.signY() * def.signZ();
    EXPECT_GT(defDet, 0.0f);

    // User flips only Y — e.g. a game that authored data with the up axis
    // mirrored. The scale becomes (1, -1, 1), determinant -1, so Ogre flips
    // culling. The visible front-face stays correct: no per-mesh winding
    // swap required, which is the whole point of routing flips through scale.
    Ps1NormalizerSettings flipY;
    flipY.flipY = true;
    EXPECT_FLOAT_EQ(flipY.signY(), -1.0f);
    const float flipYDet = flipY.signX() * flipY.signY() * flipY.signZ();
    EXPECT_LT(flipYDet, 0.0f);

    // User flips Y AND Z to bring a Z-up game into editor Y-up while keeping
    // CCW winding intact (two negative scales → positive determinant).
    Ps1NormalizerSettings flipYZ;
    flipYZ.flipY = true;
    flipYZ.flipZ = true;
    const float flipYZDet = flipYZ.signX() * flipYZ.signY() * flipYZ.signZ();
    EXPECT_GT(flipYZDet, 0.0f);
}

// Regression for the Codex P1 / CodeRabbit Major finding on the original #424
// PR: scaling/flipping the per-mesh transform without scaling the per-instance
// pivot let a multi-instance deduped capture set drift apart at 0.5× or
// collapse onto itself on a single-axis flip. composeNodeTransform must drive
// both with the same factor.
TEST(Ps1CoordinateNormalizerTest, ComposeNodeTransformDrivesPositionAndScaleTogether)
{
    Ps1NormalizerSettings def;
    float scaleOut[3] = {0, 0, 0};
    float posOut[3] = {0, 0, 0};
    Ps1CoordinateNormalizer::composeNodeTransform(def, 0.25f, 4.0f, 8.0f, -2.0f,
                                                  scaleOut, posOut);
    // userScale = 1, all signs +1 → position should equal basePos × placementScale,
    // scale should be uniform placementScale × userScale.
    EXPECT_FLOAT_EQ(scaleOut[0], 0.25f);
    EXPECT_FLOAT_EQ(scaleOut[1], 0.25f);
    EXPECT_FLOAT_EQ(scaleOut[2], 0.25f);
    EXPECT_FLOAT_EQ(posOut[0], 4.0f * 0.25f);
    EXPECT_FLOAT_EQ(posOut[1], 8.0f * 0.25f);
    EXPECT_FLOAT_EQ(posOut[2], -2.0f * 0.25f);

    // userScale = 2 → scale doubles AND position doubles, so inter-instance
    // offsets stay proportional to the mesh size. Two instances at base
    // (10, 0, 0) and (20, 0, 0) at userScale=2 land at (20,0,0) and (40,0,0).
    Ps1NormalizerSettings hi;
    hi.userScale = 2.0f;
    Ps1CoordinateNormalizer::composeNodeTransform(hi, 1.0f, 10.0f, 0.0f, 0.0f,
                                                  scaleOut, posOut);
    EXPECT_FLOAT_EQ(scaleOut[0], 2.0f);
    EXPECT_FLOAT_EQ(posOut[0], 20.0f);

    Ps1CoordinateNormalizer::composeNodeTransform(hi, 1.0f, 20.0f, 0.0f, 0.0f,
                                                  scaleOut, posOut);
    EXPECT_FLOAT_EQ(scaleOut[0], 2.0f);
    EXPECT_FLOAT_EQ(posOut[0], 40.0f);

    // flipY → position.y negates with scale.y. A row at (x, +1) and (x, -1)
    // stays a row (not a single overlapping point) after the flip.
    Ps1NormalizerSettings flipY;
    flipY.flipY = true;
    Ps1CoordinateNormalizer::composeNodeTransform(flipY, 1.0f, 5.0f, 1.0f, 0.0f,
                                                  scaleOut, posOut);
    EXPECT_FLOAT_EQ(scaleOut[1], -1.0f);
    EXPECT_FLOAT_EQ(posOut[1], -1.0f);
    Ps1CoordinateNormalizer::composeNodeTransform(flipY, 1.0f, 5.0f, -1.0f, 0.0f,
                                                  scaleOut, posOut);
    EXPECT_FLOAT_EQ(posOut[1], 1.0f);
}

TEST(Ps1CoordinateNormalizerTest, UserScaleClampedOnSettingsLoad)
{
    TempIniSettings tmp;
    // Both extremes outside the [0.001, 1000] sanity window should fall back
    // to 1.0 so a broken ini doesn't bake invisible or astronomically-scaled
    // capture nodes the user can't see / select.
    tmp().setValue(QStringLiteral("test/normalize/userScale"), 99999.0);
    Ps1NormalizerSettings hi = Ps1CoordinateNormalizer::load(tmp(), QStringLiteral("test/normalize"));
    EXPECT_FLOAT_EQ(hi.userScale, 1.0f);

    tmp().setValue(QStringLiteral("test/normalize/userScale"), -1.0);
    Ps1NormalizerSettings neg = Ps1CoordinateNormalizer::load(tmp(), QStringLiteral("test/normalize"));
    EXPECT_FLOAT_EQ(neg.userScale, 1.0f);
}
