#ifdef ENABLE_PS1_RIP

#include "Ps1AnimationExtractor.h"
#include "CaptureTypes.h"

#include <gtest/gtest.h>

#include <QVector>

namespace {

// Build a GTE record for one object-space vertex under a rotation+translation
// at a given frame. rt is passed as a flat 9-array (4.12 fixed); tr as a
// 3-array (world units). seq is monotonic across the whole capture.
GteRecordEntry rec(int16_t vx, int16_t vy, int16_t vz,
                   const int32_t rt[9], const int32_t tr[3],
                   uint32_t frame, uint32_t seq)
{
    GteRecordEntry r{};
    r.vx = vx; r.vy = vy; r.vz = vz;
    for (int i = 0; i < 9; ++i) r.rt[i] = rt[i];
    for (int i = 0; i < 3; ++i) r.tr[i] = tr[i];
    r.frame = frame;
    r.seq = seq;
    return r;
}

// Identity rotation (4.12 fixed).
const int32_t kIdentity[9] = {4096, 0, 0, 0, 4096, 0, 0, 0, 4096};

// A small triangle's three object-space verts, reused every frame (rigid).
const int16_t kTri[3][3] = {{-50, 0, 0}, {50, 0, 0}, {0, 80, 0}};

// Append one object's triangle at `frame` translated by `tr`.
void appendObject(QVector<GteRecordEntry>& out, const int32_t tr[3],
                  uint32_t frame, uint32_t& seq)
{
    for (int v = 0; v < 3; ++v)
        out.append(rec(kTri[v][0], kTri[v][1], kTri[v][2], kIdentity, tr, frame, seq++));
}

} // namespace

// A single object translating across 4 frames yields one track with 4
// frame-ordered keys whose translation advances.
TEST(Ps1AnimationExtractorTest, TranslatingObjectYieldsOneTrackWithMovingKeys)
{
    QVector<GteRecordEntry> records;
    uint32_t seq = 0;
    for (uint32_t f = 0; f < 4; ++f) {
        const int32_t tr[3] = {static_cast<int32_t>(f * 100), 0, 5000};
        appendObject(records, tr, f, seq);
    }

    const QVector<Ps1MatrixTrack> tracks = Ps1AnimationExtractor::extract(records);
    ASSERT_EQ(tracks.size(), 1);
    ASSERT_EQ(tracks[0].keys.size(), 4);
    // Frame-ordered and advancing on X.
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(tracks[0].keys[i].frame, static_cast<uint32_t>(i));
    EXPECT_LT(tracks[0].keys[0].tr[0], tracks[0].keys[3].tr[0]);
}

// A static object (same matrix every frame) is dropped when requireMotion is
// on (the default) and kept when it's off.
TEST(Ps1AnimationExtractorTest, StaticObjectDroppedUnlessMotionNotRequired)
{
    QVector<GteRecordEntry> records;
    uint32_t seq = 0;
    const int32_t tr[3] = {0, 0, 5000};
    for (uint32_t f = 0; f < 5; ++f)
        appendObject(records, tr, f, seq);

    EXPECT_TRUE(Ps1AnimationExtractor::extract(records).isEmpty()); // requireMotion default

    Ps1AnimationExtractor::Options keepStatic;
    keepStatic.requireMotion = false;
    const QVector<Ps1MatrixTrack> tracks = Ps1AnimationExtractor::extract(records, keepStatic);
    ASSERT_EQ(tracks.size(), 1);
    EXPECT_EQ(tracks[0].keys.size(), 5);
}

// Two distinct objects (different vertex sets), one moving and one static,
// each moving over the same frames: only the moving one produces a track by
// default; both do when motion isn't required.
TEST(Ps1AnimationExtractorTest, TwoObjectsTrackedIndependently)
{
    QVector<GteRecordEntry> records;
    uint32_t seq = 0;
    // Object A: the standard triangle, translating.
    // Object B: a DIFFERENT triangle (distinct verts), static.
    const int16_t triB[3][3] = {{200, 0, 0}, {300, 0, 0}, {250, 90, 0}};
    for (uint32_t f = 0; f < 3; ++f) {
        // A starts at x=500 and moves right, so it never coincides with B.
        const int32_t trA[3] = {static_cast<int32_t>(500 + f * 120), 0, 6000};
        appendObject(records, trA, f, seq);
        const int32_t trB[3] = {-500, 0, 6000}; // static, well away from A
        for (int v = 0; v < 3; ++v)
            records.append(rec(triB[v][0], triB[v][1], triB[v][2], kIdentity, trB, f, seq++));
    }

    // Default: only the moving object A.
    const QVector<Ps1MatrixTrack> moving = Ps1AnimationExtractor::extract(records);
    ASSERT_EQ(moving.size(), 1);
    EXPECT_EQ(moving[0].keys.size(), 3);

    // Motion not required: both objects.
    Ps1AnimationExtractor::Options all;
    all.requireMotion = false;
    all.minFrames = 1;
    const QVector<Ps1MatrixTrack> both = Ps1AnimationExtractor::extract(records, all);
    EXPECT_EQ(both.size(), 2);
}

// minFrames gates single-frame objects (a prop drawn once is not animation).
TEST(Ps1AnimationExtractorTest, MinFramesGatesSingleFrameObjects)
{
    QVector<GteRecordEntry> records;
    uint32_t seq = 0;
    const int32_t tr[3] = {0, 0, 4000};
    appendObject(records, tr, 0, seq); // only frame 0

    EXPECT_TRUE(Ps1AnimationExtractor::extract(records).isEmpty()); // minFrames=2 default
}

// Empty input → empty output (no crash).
TEST(Ps1AnimationExtractorTest, EmptyInputYieldsNoTracks)
{
    EXPECT_TRUE(Ps1AnimationExtractor::extract({}).isEmpty());
}

// matricesDiffer: identical → false; translation past epsilon → true.
TEST(Ps1AnimationExtractorTest, MatricesDifferDetectsMotion)
{
    const int32_t tr0[3] = {0, 0, 0};
    const int32_t tr1[3] = {5, 0, 0};
    EXPECT_FALSE(Ps1AnimationExtractor::matricesDiffer(kIdentity, tr0, kIdentity, tr0));
    EXPECT_TRUE(Ps1AnimationExtractor::matricesDiffer(kIdentity, tr0, kIdentity, tr1));
}

#endif // ENABLE_PS1_RIP
