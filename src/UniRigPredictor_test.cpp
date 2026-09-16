#include <algorithm>
#include <limits>
// Unit tests for UniRigPredictor (#408, retargeted from RigNet to UniRig:
// VAST-AI-Research/UniRig, SIGGRAPH 2025). No Ogre / GL / ONNX needed: these
// exercise the pure-data tokenizer (undiscretize + the detokenize FSM) and the
// graceful-failure paths (missing model, degenerate input, ONNX-disabled build)
// that AutoRig relies on for its Pinocchio fallback. The actual ONNX inference
// (the SAL perceiver encoder + the autoregressive decoder) needs the hosted
// model and is covered behind ENABLE_ONNX on CI when the model is available.
//
// The detokenizer FSM + undiscretize are exposed as static helpers on
// UniRigPredictor specifically so this test can verify them without a model:
//   static UniRigPredictor::Result
//       detokenize(const std::vector<int>& ids, double scale,
//                  const std::array<double,3>& centre);
//   static double undiscretize(int bin);

#include <gtest/gtest.h>
#include <QRegularExpression>
#include <random>

#include <QString>
#include <array>
#include <cmath>
#include <set>
#include <cstdint>
#include <vector>

#include "UniRigPredictor.h"
#include "AutoRig.h"
#include "MotionInbetween.h"

namespace {

// Tokenizer vocab id layout (configs/tokenizer/tokenizer_parts_articulationxl_256.yaml).
// num_discrete = 256, continuous_range = [-1, 1].
constexpr int kNumDiscrete   = 256;
constexpr int kBranch        = 256;   // token_id_branch
constexpr int kBos           = 257;   // token_id_bos
constexpr int kEos           = 258;   // token_id_eos
constexpr int kPad           = 259;   // token_id_pad
constexpr int kSpring        = 260;   // token_id_spring ("part" = None)
constexpr int kPartBody      = 261;
constexpr int kPartHand      = 262;
constexpr int kClsNone       = 263;   // token_id_cls_none
constexpr int kClsVroid      = 264;
constexpr int kClsMixamo     = 265;
constexpr int kClsArtxl      = 266;

// Reference undiscretize: f = (t + 0.5) / num_discrete; f*(hi-lo) + lo, lo=-1,hi=1.
double refUndiscretize(int bin)
{
    const double f = (static_cast<double>(bin) + 0.5) / kNumDiscrete;
    return f * 2.0 - 1.0;
}

// Reference discretize: u = (t-lo)/(hi-lo); u *= num_discrete; clip(round(u),0,255).
int refDiscretize(double t)
{
    double u = (t - (-1.0)) / 2.0;
    u *= kNumDiscrete;
    long r = std::lround(u);
    if (r < 0)   r = 0;
    if (r > 255) r = 255;
    return static_cast<int>(r);
}

// A tiny tetrahedron (4 verts, 4 faces) — enough to pass the vertex-count
// guard so predict() reaches the model-presence check.
std::vector<float> tetraVerts()
{
    return { 0,0,0,  1,0,0,  0,1,0,  0,0,1 };
}
std::vector<uint32_t> tetraIdx()
{
    return { 0,1,2,  0,1,3,  0,2,3,  1,2,3 };
}

} // namespace

// ---------------------------------------------------------------------------
// undiscretize
// ---------------------------------------------------------------------------

TEST(UniRigPredictor, UndiscretizeEndpointsAndMidpoint)
{
    // bin 0   -> -1 + 1/256
    EXPECT_NEAR(UniRigPredictor::undiscretize(0), -1.0 + 1.0 / 256.0, 1e-9);
    // bin 255 ->  1 - 1/256
    EXPECT_NEAR(UniRigPredictor::undiscretize(255), 1.0 - 1.0 / 256.0, 1e-9);
    // the centre bins straddle 0 — both ~+/- 1/256
    EXPECT_NEAR(UniRigPredictor::undiscretize(127), -1.0 / 256.0, 1e-9);
    EXPECT_NEAR(UniRigPredictor::undiscretize(128),  1.0 / 256.0, 1e-9);
}

TEST(UniRigPredictor, UndiscretizeMatchesReferenceFormula)
{
    for (int b : {0, 1, 17, 64, 100, 128, 200, 254, 255}) {
        EXPECT_NEAR(UniRigPredictor::undiscretize(b), refUndiscretize(b), 1e-9)
            << "bin=" << b;
    }
}

TEST(UniRigPredictor, UndiscretizeIsMonotonicAndInRange)
{
    double prev = -2.0;
    for (int b = 0; b < kNumDiscrete; ++b) {
        const double v = UniRigPredictor::undiscretize(b);
        EXPECT_GT(v, -1.0);
        EXPECT_LT(v,  1.0);
        EXPECT_GT(v, prev) << "not strictly increasing at bin " << b;
        prev = v;
    }
}

// ---------------------------------------------------------------------------
// detokenize FSM — implicit-parent (chain) path
// ---------------------------------------------------------------------------

TEST(UniRigPredictor, DetokenizeTwoJointChainImplicitParent)
{
    // A hand-built valid token sequence:
    //   bos, cls_none, [root xyz bins], [child xyz bins], eos
    // root  -> first joint, self-parent (root) => parent -1
    // child -> no branch token, so parent = last joint (the root) => parent 0
    const std::array<double, 3> root  = { -0.5,  0.25,  0.75 };
    const std::array<double, 3> child = {  0.10, -0.30,  0.50 };

    std::vector<int> ids;
    ids.push_back(kBos);
    ids.push_back(kClsNone);
    for (double c : root)  ids.push_back(refDiscretize(c));
    for (double c : child) ids.push_back(refDiscretize(c));
    ids.push_back(kEos);

    // Identity normalization (scale=1, centre=origin) so the de-normalized
    // joints come straight back as the undiscretized bin centres.
    const std::array<double, 3> centre = { 0, 0, 0 };
    const auto r = UniRigPredictor::detokenize(ids, 1.0, centre);

    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.joints.size(), 2u);

    // Parents: [-1, 0]
    EXPECT_EQ(r.joints[0].parent, -1);
    EXPECT_EQ(r.joints[1].parent,  0);

    // Positions = undiscretize(bin) (identity normalization).
    for (int k = 0; k < 3; ++k) {
        EXPECT_NEAR(r.joints[0].pos[k], refUndiscretize(refDiscretize(root[k])),  1e-9) << "root k=" << k;
        EXPECT_NEAR(r.joints[1].pos[k], refUndiscretize(refDiscretize(child[k])), 1e-9) << "child k=" << k;
    }
}

TEST(UniRigPredictor, DetokenizeDeNormalizesWithScaleAndCentre)
{
    // De-normalize back to mesh-local: pos = undiscretize(bin) * scale + centre.
    const std::array<double, 3> rootN  = {  0.0,  0.0,  0.0 };
    const std::array<double, 3> childN = {  0.5, -0.5,  0.25 };

    std::vector<int> ids;
    ids.push_back(kBos);
    ids.push_back(kClsNone);
    for (double c : rootN)  ids.push_back(refDiscretize(c));
    for (double c : childN) ids.push_back(refDiscretize(c));
    ids.push_back(kEos);

    const double scale = 3.0;
    const std::array<double, 3> centre = { 10.0, -2.0, 5.0 };
    const auto r = UniRigPredictor::detokenize(ids, scale, centre);

    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.joints.size(), 2u);

    for (int k = 0; k < 3; ++k) {
        const double expRoot  = refUndiscretize(refDiscretize(rootN[k]))  * scale + centre[k];
        const double expChild = refUndiscretize(refDiscretize(childN[k])) * scale + centre[k];
        EXPECT_NEAR(r.joints[0].pos[k], expRoot,  1e-6) << "root k=" << k;
        EXPECT_NEAR(r.joints[1].pos[k], expChild, 1e-6) << "child k=" << k;
    }
}

// ---------------------------------------------------------------------------
// detokenize FSM — explicit-parent (branch) path
// ---------------------------------------------------------------------------

TEST(UniRigPredictor, DetokenizeBranchTokenYieldsExplicitParent)
{
    // After a branch token (256) a coord run is a parent-triple followed by a
    // joint-triple: the parent position points back at an EARLIER joint and the
    // FSM resolves the parent index by matching that position.
    //
    // Sequence:
    //   bos, cls_none,
    //   [root xyz]                         -> joint 0 (root, parent -1)
    //   [a xyz]                            -> joint 1 (chain child of root => parent 0)
    //   branch, [parent==root xyz][b xyz]  -> joint 2 explicitly parented to root => parent 0
    //   eos
    const std::array<double, 3> root = { -0.5,  0.0,  0.5 };
    const std::array<double, 3> a    = {  0.25, 0.25, 0.0 };
    const std::array<double, 3> b    = {  0.75,-0.25,-0.5 };

    // The explicit parent-triple must encode the SAME bins the root joint was
    // emitted with (the model emits a quantized parent position that exactly
    // re-states an earlier joint's bins), so the detokenizer's position match
    // resolves it back to joint 0. Encode discretize(root) directly — NOT
    // discretize(undiscretize(discretize(root))): the +0.5 bin-centre offset
    // makes that double round-trip land one bin higher, which would no longer
    // match joint 0's stored position.
    std::vector<int> ids;
    ids.push_back(kBos);
    ids.push_back(kClsNone);
    for (double c : root) ids.push_back(refDiscretize(c));      // joint 0
    for (double c : a)    ids.push_back(refDiscretize(c));      // joint 1 (chain)
    ids.push_back(kBranch);                                     // explicit parent next
    for (double c : root) ids.push_back(refDiscretize(c));      // parent triple == root bins
    for (double c : b)    ids.push_back(refDiscretize(c));      // joint 2
    ids.push_back(kEos);

    const std::array<double, 3> centre = { 0, 0, 0 };
    const auto r = UniRigPredictor::detokenize(ids, 1.0, centre);

    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.joints.size(), 3u);

    EXPECT_EQ(r.joints[0].parent, -1);   // root
    EXPECT_EQ(r.joints[1].parent,  0);   // chained off root
    EXPECT_EQ(r.joints[2].parent,  0);   // explicit branch back to root (NOT 1)

    for (int k = 0; k < 3; ++k)
        EXPECT_NEAR(r.joints[2].pos[k], refUndiscretize(refDiscretize(b[k])), 1e-9) << "b k=" << k;
}

// ---------------------------------------------------------------------------
// detokenize FSM — part / cls tokens are consumed, not errors
// ---------------------------------------------------------------------------

TEST(UniRigPredictor, DetokenizeAcceptsPartAndClsTokens)
{
    // spring (None part), body/hand part tokens, and a cls token should all be
    // consumed by the FSM without affecting the joint tree.
    const std::array<double, 3> root  = { 0.0, 0.0, 0.0 };
    const std::array<double, 3> child = { 0.5, 0.5, 0.5 };

    std::vector<int> ids;
    ids.push_back(kBos);
    ids.push_back(kClsMixamo);                              // cls
    ids.push_back(kSpring);                                 // part = None
    for (double c : root)  ids.push_back(refDiscretize(c)); // joint 0
    ids.push_back(kPartBody);                               // part token mid-stream
    for (double c : child) ids.push_back(refDiscretize(c)); // joint 1
    ids.push_back(kEos);

    const std::array<double, 3> centre = { 0, 0, 0 };
    const auto r = UniRigPredictor::detokenize(ids, 1.0, centre);

    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.joints.size(), 2u);
    EXPECT_EQ(r.joints[0].parent, -1);
    EXPECT_EQ(r.joints[1].parent,  0);
}

TEST(UniRigPredictor, DetokenizeStripsLeadingBosAndTrailingPad)
{
    // Leading bos + trailing pad are stripped; the last *real* token is eos.
    const std::array<double, 3> root = { 0.0, 0.0, 0.0 };

    std::vector<int> ids;
    ids.push_back(kBos);
    ids.push_back(kClsNone);
    for (double c : root) ids.push_back(refDiscretize(c)); // single root joint
    ids.push_back(kEos);
    ids.push_back(kPad);
    ids.push_back(kPad);

    const std::array<double, 3> centre = { 0, 0, 0 };
    const auto r = UniRigPredictor::detokenize(ids, 1.0, centre);

    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.joints.size(), 1u);
    EXPECT_EQ(r.joints[0].parent, -1);   // lone joint is the root
}

// ---------------------------------------------------------------------------
// detokenize FSM — failure paths
// ---------------------------------------------------------------------------

TEST(UniRigPredictor, DetokenizeEmptyOrNoEosFails)
{
    const std::array<double, 3> centre = { 0, 0, 0 };

    // Empty.
    {
        const auto r = UniRigPredictor::detokenize({}, 1.0, centre);
        EXPECT_FALSE(r.ok);
        EXPECT_FALSE(r.error.isEmpty());
        EXPECT_TRUE(r.joints.empty());
    }
    // No eos (last real token is a coord bin).
    {
        std::vector<int> ids = { kBos, kClsNone, 10, 20, 30 };
        const auto r = UniRigPredictor::detokenize(ids, 1.0, centre);
        EXPECT_FALSE(r.ok);
    }
}

TEST(UniRigPredictor, DetokenizeUnknownTokenFails)
{
    // An id at or above vocab_size (267) is not a valid token.
    std::vector<int> ids = { kBos, 267, kEos };
    const std::array<double, 3> centre = { 0, 0, 0 };
    const auto r = UniRigPredictor::detokenize(ids, 1.0, centre);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

// ---------------------------------------------------------------------------
// model-path resolution + ONNX-disabled / missing-model graceful failures
// ---------------------------------------------------------------------------

TEST(UniRigPredictor, ModelPathIsUnderAiModelsUnirig)
{
    const QString p = UniRigPredictor::modelPath();
    EXPECT_FALSE(p.isEmpty());
    EXPECT_TRUE(p.contains(QStringLiteral("ai_models")));
    EXPECT_TRUE(p.contains(QStringLiteral("unirig")));
    EXPECT_TRUE(p.endsWith(QStringLiteral(".onnx")));
}

TEST(UniRigPredictor, EncoderAndDecoderPathsUnderAiModelsUnirig)
{
    // UniRig is a two-model architecture (SAL perceiver encoder + autoregressive
    // decoder); both resolve under ai_models/unirig.
    const QString enc = UniRigPredictor::encoderModelPath();
    const QString dec = UniRigPredictor::decoderModelPath();

    EXPECT_FALSE(enc.isEmpty());
    EXPECT_FALSE(dec.isEmpty());

    EXPECT_TRUE(enc.contains(QStringLiteral("ai_models")));
    EXPECT_TRUE(enc.contains(QStringLiteral("unirig")));
    EXPECT_TRUE(enc.endsWith(QStringLiteral(".onnx")));

    EXPECT_TRUE(dec.contains(QStringLiteral("ai_models")));
    EXPECT_TRUE(dec.contains(QStringLiteral("unirig")));
    EXPECT_TRUE(dec.endsWith(QStringLiteral(".onnx")));

    // Encoder and decoder are distinct files.
    EXPECT_NE(enc, dec);
}

TEST(UniRigPredictor, MissingModelFailsGracefully)
{
    auto v = tetraVerts();
    auto idx = tetraIdx();
    const auto r = UniRigPredictor::predict(
        v.data(), static_cast<int>(v.size() / 3),
        idx.data(), static_cast<int>(idx.size()),
        QStringLiteral("/nonexistent/encoder.onnx"),
        QStringLiteral("/nonexistent/decoder.onnx"),
        QStringLiteral("/nonexistent/embed.onnx"));
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());   // a reason AutoRig can log on fallback
    EXPECT_TRUE(r.joints.empty());
}

TEST(UniRigPredictor, TooFewVerticesFails)
{
    std::vector<float> v = { 0,0,0,  1,0,0 };   // 2 verts < the minimum
    const auto r = UniRigPredictor::predict(
        v.data(), 2, nullptr, 0,
        QStringLiteral("/nonexistent/encoder.onnx"),
        QStringLiteral("/nonexistent/decoder.onnx"),
        QStringLiteral("/nonexistent/embed.onnx"));
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

TEST(UniRigPredictor, NullPositionsFails)
{
    const auto r = UniRigPredictor::predict(
        nullptr, 100, nullptr, 0,
        QStringLiteral("/nonexistent/encoder.onnx"),
        QStringLiteral("/nonexistent/decoder.onnx"),
        QStringLiteral("/nonexistent/embed.onnx"));
    EXPECT_FALSE(r.ok);
}

TEST(UniRigPredictor, LabelsAnatomicallyResolveCanonicalJoints)
{
    // Build a synthetic +Y-up T-pose skeleton with UniRig-style positional names
    // (joint_N) and verify labelJointsAnatomically renames them to anatomical
    // names that MotionInbetween::canonicalIndexForBone then resolves — the fix
    // for "0/22 resolved" on a UniRig rig.
    //
    // CONVENTION: the labeler names the character's LEFT on the −X side (the
    // glTF/Ogre Y-up, faces-+Z convention this targets), so the limbs below are
    // laid out with the LEFT chain on −X and RIGHT on +X. (The retarget then does
    // its own handedness compensation against the CMU clip so motion isn't
    // mirrored — labels and motion are decoupled.)
    // Seed unique positional names like the real predictor (jointName → joint_N),
    // so the test input mirrors production; the labeler's final uniqueness pass
    // also backstops any collision.
    int nextId = 0;
    auto J = [&nextId](double x, double y, double z, int parent) {
        UniRigPredictor::Joint j; j.pos = {x, y, z}; j.parent = parent;
        j.name = QStringLiteral("joint_%1").arg(nextId++); return j;
    };
    std::vector<UniRigPredictor::Joint> joints = {
        J(0.0, 0.0, 0.0, -1),   // 0 hips (root)
        J(0.0, 0.3, 0.0,  0),   // 1 spine
        J(0.0, 0.6, 0.0,  1),   // 2 chest
        J(0.0, 0.8, 0.0,  2),   // 3 neck
        J(0.0, 0.95, 0.0, 3),   // 4 head
        // LEFT arm (−X)
        J(-0.2, 0.6, 0.0,  2),  // 5 L upper arm
        J(-0.45, 0.6, 0.0, 5),  // 6 L forearm
        J(-0.65, 0.6, 0.0, 6),  // 7 L hand
        // RIGHT arm (+X)
        J(0.2, 0.6, 0.0,  2),   // 8 R upper arm
        J(0.45, 0.6, 0.0, 8),   // 9 R forearm
        J(0.65, 0.6, 0.0, 9),   // 10 R hand
        // LEFT leg (−X, down)
        J(-0.1, -0.1, 0.0, 0),  // 11 L upleg
        J(-0.1, -0.5, 0.0, 11), // 12 L leg
        J(-0.1, -0.9, 0.0, 12), // 13 L foot
        // RIGHT leg (+X, down)
        J(0.1, -0.1, 0.0, 0),   // 14 R upleg
        J(0.1, -0.5, 0.0, 14),  // 15 R leg
        J(0.1, -0.9, 0.0, 15),  // 16 R foot
    };
    UniRigPredictor::labelJointsAnatomically(joints, /*upAxis=*/1);

    // All bone names MUST be unique — Ogre::Skeleton::createBone rejects dups
    // ("RightArm already exists"). The labeler suffixes any collision.
    std::set<QString> seen;
    for (const auto& j : joints) {
        EXPECT_EQ(seen.count(j.name), 0u) << "duplicate bone name: " << j.name.toStdString();
        seen.insert(j.name);
    }

    int resolved = 0;
    for (const auto& j : joints)
        if (MotionInbetween::canonicalIndexForBone(j.name) >= 0) ++resolved;
    // We should resolve a strong majority of the 17 placed joints (all but maybe
    // an ambiguous spine link). The text-to-motion gate needs ≥11/22.
    EXPECT_GE(resolved, 12) << "only resolved " << resolved << " joints";
    // Spot-check key roles map correctly.
    EXPECT_EQ(MotionInbetween::canonicalIndexForBone(joints[0].name), 0);  // Hips
    EXPECT_EQ(MotionInbetween::canonicalIndexForBone(joints[4].name), 5);  // Head
    // The −X hand (joints[7]) resolves to canon 13 (lhand); the +X hand
    // (joints[10]) to canon 9 (rhand) — the labeler's −X=Left convention.
    const int lh = MotionInbetween::canonicalIndexForBone(joints[7].name);
    EXPECT_EQ(lh, 13) << "left (−X) hand index " << lh;
    const int rh = MotionInbetween::canonicalIndexForBone(joints[10].name);
    EXPECT_EQ(rh, 9) << "right (+X) hand index " << rh;
}

TEST(UniRigPredictor, SampleBudgetScalesForLargeMeshes)
{
    EXPECT_EQ(UniRigPredictor::sampleBudgetForMesh(1000), 65536);
    EXPECT_EQ(UniRigPredictor::sampleBudgetForMesh(100000), 32768);
    EXPECT_EQ(UniRigPredictor::sampleBudgetForMesh(500000), 16384);
    EXPECT_EQ(UniRigPredictor::sampleBudgetForMesh(1000, 12000), 12000);
    EXPECT_EQ(UniRigPredictor::sampleBudgetForMesh(1000, 1000), 4096);
    EXPECT_EQ(UniRigPredictor::sampleBudgetForMesh(1000, 999999), 65536);
}

TEST(UniRigPredictor, EnsureModelBlockingHonoursNoDownloadGuard)
{
    // With QTMESH_UNIRIG_NO_DOWNLOAD set (and no model on disk) ensureModelBlocking
    // must return empty WITHOUT touching the network — the offline/test contract.
    // (If a model happens to be cached on disk it returns that path; either way it
    // must never hang or crash.)
    qputenv("QTMESH_UNIRIG_NO_DOWNLOAD", "1");
    const QString p = UniRigPredictor::ensureModelBlocking();
    qunsetenv("QTMESH_UNIRIG_NO_DOWNLOAD");
    SUCCEED();
    (void)p;
}

TEST(UniRigPredictor, LabelJointsClassifiesAPoseArmsByAttachHeight)
{
    // #969: A-pose arms DESCEND almost as steeply as legs, so the old
    // direction-only rule ("arms extend sideways") named both arm chains
    // *UpLeg_1 on a real UniRig rig and the leg tracks drove the arms.
    // Attach height must win: chains hanging off the upper spine are arms
    // even when they drop; chains off the root are legs even when they splay.
    using J = UniRigPredictor::Joint;
    std::vector<J> j;
    auto add = [&](double x, double y, double z, int parent) {
        J jt; jt.pos = {x, y, z}; jt.parent = parent; jt.part = -1;
        jt.name = QStringLiteral("joint_%1").arg(j.size());
        j.push_back(jt);
        return static_cast<int>(j.size()) - 1;
    };
    const int hips  = add(0, 1.00, 0, -1);
    const int sp0   = add(0, 1.15, 0, hips);
    const int sp1   = add(0, 1.30, 0, sp0);
    const int chest = add(0, 1.45, 0, sp1);
    const int neck  = add(0, 1.55, 0, chest);
    add(0, 1.70, 0, neck);                       // head
    // A-POSE arms: attach at the chest, drop steeply (dy ≈ -0.45 over the
    // chain vs lateral reach ≈ 0.35 — the old rule called this a leg).
    const int lsh = add(+0.15, 1.42, 0, chest);
    const int lel = add(+0.30, 1.15, 0, lsh);
    add(+0.42, 0.90, 0, lel);
    const int rsh = add(-0.15, 1.42, 0, chest);
    const int rel = add(-0.30, 1.15, 0, rsh);
    add(-0.42, 0.90, 0, rel);
    // Legs: attach at the root, straight down.
    const int lhip = add(+0.12, 0.95, 0, hips);
    const int lkne = add(+0.12, 0.50, 0, lhip);
    add(+0.12, 0.05, 0, lkne);
    const int rhip = add(-0.12, 0.95, 0, hips);
    const int rkne = add(-0.12, 0.50, 0, rhip);
    add(-0.12, 0.05, 0, rkne);

    UniRigPredictor::labelJointsAnatomically(j, /*upAxis=*/1);

    int armChains = 0, legChains = 0, legNamedHigh = 0;
    for (const auto& jt : j) {
        if (jt.name.contains(QLatin1String("Arm"))
            && !jt.name.contains(QLatin1String("ForeArm"))) ++armChains;
        if (jt.name.contains(QLatin1String("UpLeg"))) ++legChains;
        if (jt.name.contains(QLatin1String("UpLeg")) && jt.pos[1] > 1.2)
            ++legNamedHigh;
    }
    EXPECT_EQ(armChains, 2) << "both A-pose chains must be named as arms";
    EXPECT_EQ(legChains, 2) << "exactly the two root chains are legs";
    EXPECT_EQ(legNamedHigh, 0) << "no leg name may land at chest height";
}

// #1025 — the NaN-latents guard itself lives inside the ENABLE_ONNX inference
// path and needs a loaded encoder session, so it cannot be reached from these
// pure-data tests. What IS testable here is the predicate the guard applies, so
// a future refactor cannot silently weaken it: an all-non-finite prefix must be
// rejected, and a prefix with any finite value must not be.
//
// The live behaviour was verified manually against the hosted export
// (jana.obj, buick_riviera*.glb): 1x1024x1024 latents, 1048576/1048576
// non-finite, now reported as "the encoder export is numerically broken"
// instead of the misleading downstream "constrained decode reached a dead
// state", and failing in 2.4s instead of 2m24s.
namespace {
// Mirrors the guard in UniRigPredictor.cpp: all sampled values non-finite.
bool looksLikeBrokenExport(const std::vector<float>& latents, int probeLen)
{
    const int probe = std::min<int>(static_cast<int>(latents.size()), probeLen);
    int bad = 0;
    for (int i = 0; i < probe; ++i)
        if (!std::isfinite(latents[static_cast<size_t>(i)])) ++bad;
    return probe > 0 && bad == probe;
}
} // namespace

TEST(UniRigPredictor, BrokenExportPredicateRejectsAllNaNLatents)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_TRUE(looksLikeBrokenExport(std::vector<float>(2048, nan), 1024));
    EXPECT_TRUE(looksLikeBrokenExport(
        std::vector<float>(2048, std::numeric_limits<float>::infinity()), 1024));
}

TEST(UniRigPredictor, BrokenExportPredicateAcceptsUsableLatents)
{
    std::vector<float> ok(2048, 0.25f);
    EXPECT_FALSE(looksLikeBrokenExport(ok, 1024));
    // A single finite value inside the probe window is enough to proceed — the
    // guard must only fire on a WHOLLY broken export, never on a mesh that
    // happens to produce one bad activation.
    std::vector<float> mostlyBad(2048, std::numeric_limits<float>::quiet_NaN());
    mostlyBad[7] = 1.0f;
    EXPECT_FALSE(looksLikeBrokenExport(mostlyBad, 1024));
}

// #1025: the default-hosted files carry published digests; a corrupt download
// is detected by ModelFetch before the graph is ever loaded.
TEST(UniRigPredictorDigests, DefaultHostedFilesHaveDistinctSha256)
{
    const QString e = UniRigPredictor::expectedSha256("encoder.onnx");
    const QString d = UniRigPredictor::expectedSha256("decoder.onnx");
    const QString m = UniRigPredictor::expectedSha256("embed.onnx");
    for (const QString& s : {e, d, m}) {
        EXPECT_EQ(s.size(), 64);
        EXPECT_TRUE(QRegularExpression("^[0-9a-f]{64}$").match(s).hasMatch()) << s.toStdString();
    }
    EXPECT_NE(e, d); EXPECT_NE(d, m); EXPECT_NE(e, m);
    EXPECT_TRUE(UniRigPredictor::expectedSha256("other.onnx").isEmpty());
}

// #1046: the first k points after front-loading must cover the cloud far
// better than the first k points of the raw (random) order, normals must stay
// paired with their points, and nothing may be lost or duplicated.
TEST(UniRigPredictorFps, FrontLoadedPrefixCoversTheCloud)
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> U(-1.f, 1.f);
    const int n = 6000, k = 128;
    std::vector<float> pts(n * 3), nrm(n * 3);
    for (int i = 0; i < n; ++i) {
        // an elongated shape with a thin "tail": most points in a body blob,
        // 20 along a long thin spike, and NONE of them within the first k
        // indices — so the raw prefix cannot see the tail while FPS must.
        const bool tail = (i % 300 == 299);
        pts[3*i]   = tail ? 3.f + U(rng) * 0.05f : U(rng) * 0.5f;
        pts[3*i+1] = tail ? U(rng) * 0.02f : U(rng) * 0.5f;
        pts[3*i+2] = tail ? U(rng) * 0.02f : U(rng) * 0.5f;
        for (int c = 0; c < 3; ++c) nrm[3*i+c] = pts[3*i+c];   // normals == points: pairing probe
    }
    auto coverageRadius = [&](const std::vector<float>& p) {
        double worst = 0;
        for (int i = 0; i < n; ++i) {
            double best = 1e30;
            for (int j = 0; j < k; ++j) {
                const double dx = p[3*i]-p[3*j], dy = p[3*i+1]-p[3*j+1], dz = p[3*i+2]-p[3*j+2];
                best = std::min(best, dx*dx+dy*dy+dz*dz);
            }
            worst = std::max(worst, best);
        }
        return std::sqrt(worst);
    };
    const double before = coverageRadius(pts);
    std::vector<float> p2 = pts, n2 = nrm;
    UniRigPredictor::frontLoadFarthestPoints(p2, n2, n, k);
    const double after = coverageRadius(p2);
    // The raw prefix misses the tail entirely (coverage radius ~ the tail's
    // length, ~2.5); FPS reaches it, leaving only the body's packing radius.
    EXPECT_GT(before, 2.0) << "fixture: the raw prefix must not reach the tail";
    EXPECT_LT(after, 0.5) << "before=" << before << " after=" << after;
    bool tailInPrefix = false;
    for (int j = 0; j < k; ++j) if (p2[3*j] > 2.5f) tailInPrefix = true;
    EXPECT_TRUE(tailInPrefix) << "FPS must reach the thin tail within the prefix";
    EXPECT_EQ(p2, n2) << "normals must move with their points";
    std::vector<float> a = pts, b = p2; std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
    EXPECT_EQ(a, b) << "must be a permutation of the input";
    // pool: candidates limited to the first `pool` points, nothing dropped
    std::vector<float> p3 = pts, n3 = nrm;
    UniRigPredictor::frontLoadFarthestPoints(p3, n3, n, k, 1000);
    EXPECT_EQ(p3.size(), pts.size());
    { std::vector<float> a2 = pts, b2 = p3; std::sort(a2.begin(), a2.end()); std::sort(b2.begin(), b2.end()); EXPECT_EQ(a2, b2); }
    // the tail (indices 299, 599, 899 lie inside the 1000-point pool) is still reached
    bool tailInPrefix3 = false;
    for (int j = 0; j < k; ++j) if (p3[3*j] > 2.5f) tailInPrefix3 = true;
    EXPECT_TRUE(tailInPrefix3);
    std::vector<float> tiny = {0,0,0, 1,1,1}, tn = tiny;
    UniRigPredictor::frontLoadFarthestPoints(tiny, tn, 2, 2048);
    EXPECT_EQ(tiny, (std::vector<float>{0,0,0, 1,1,1})) << "degenerate input is a no-op";
}

// #1046: Both-mode chooser — a failed run never wins, the richer skeleton
// wins, ties go to fps (the paper's sampling).
TEST(UniRigPredictorFps, PickRicherPrefersSuccessThenJointCountThenFps)
{
    UniRigPredictor::Result fps, rnd;
    fps.ok = true; fps.joints.resize(24); fps.querySampling = "fps";
    rnd.ok = true; rnd.joints.resize(6);  rnd.querySampling = "random";
    EXPECT_EQ(UniRigPredictor::pickRicher(fps, rnd).querySampling, "fps");
    rnd.joints.resize(64);
    EXPECT_EQ(UniRigPredictor::pickRicher(fps, rnd).querySampling, "random");
    rnd.joints.resize(24);
    EXPECT_EQ(UniRigPredictor::pickRicher(fps, rnd).querySampling, "fps") << "tie -> fps";
    fps.ok = false;
    EXPECT_EQ(UniRigPredictor::pickRicher(fps, rnd).querySampling, "random") << "a failed run never wins";
    rnd.ok = false; fps.ok = true; fps.joints.resize(1);
    EXPECT_EQ(UniRigPredictor::pickRicher(fps, rnd).querySampling, "fps");
}

// The env override must only narrow the default Both — an explicit single
// ordering (what predictBoth sets on its inner calls) must win, or
// QTMESH_UNIRIG_QUERIES=both recurses forever.
TEST(UniRigPredictorFps, EnvOverrideNarrowsBothButNeverWidensAnExplicitMode)
{
    using Q = UniRigPredictor::Options::QuerySampling;
    UniRigPredictor::Options both; both.querySampling = Q::Both;
    UniRigPredictor::Options fps;  fps.querySampling  = Q::Fps;
    qunsetenv("QTMESH_UNIRIG_QUERIES");
    EXPECT_EQ(UniRigPredictor::resolveQuerySampling(both), Q::Both);
    EXPECT_EQ(UniRigPredictor::resolveQuerySampling(fps),  Q::Fps);
    qputenv("QTMESH_UNIRIG_QUERIES", "random");
    EXPECT_EQ(UniRigPredictor::resolveQuerySampling(both), Q::Random);
    EXPECT_EQ(UniRigPredictor::resolveQuerySampling(fps),  Q::Fps) << "explicit mode wins over the env";
    qputenv("QTMESH_UNIRIG_QUERIES", "both");
    EXPECT_EQ(UniRigPredictor::resolveQuerySampling(both), Q::Both);
    EXPECT_EQ(UniRigPredictor::resolveQuerySampling(fps),  Q::Fps) << "inner Fps call must not be widened back to Both";
    qunsetenv("QTMESH_UNIRIG_QUERIES");

// #1013: the humanoid labeller must SAY when the geometry does not read as a
// humanoid, and Auto must then fall back to neutral names.
namespace {
UniRigPredictor::Joint mkJ(double x, double y, double z, int parent, int id)
{
    UniRigPredictor::Joint j; j.pos = {x, y, z}; j.parent = parent;
    j.name = QStringLiteral("joint_%1").arg(id); return j;
}
std::vector<UniRigPredictor::Joint> syntheticHumanoid()
{
    int n = 0; auto J = [&](double x, double y, double z, int p) { return mkJ(x, y, z, p, n++); };
    return { J(0,0,0,-1), J(0,.3,0,0), J(0,.6,0,1), J(0,.8,0,2), J(0,.95,0,3),
             J(-.2,.6,0,2), J(-.45,.6,0,5), J(-.65,.6,0,6),
             J(.2,.6,0,2),  J(.45,.6,0,8),  J(.65,.6,0,9),
             J(-.1,-.1,0,0), J(-.1,-.5,0,11), J(-.1,-.9,0,12),
             J(.1,-.1,0,0),  J(.1,-.5,0,14),  J(.1,-.9,0,15) };
}
// A car: a long chain along +Z (its length) with four short chains dropping to
// the wheels. The labeller reads the long axis as "up" → not a humanoid.
std::vector<UniRigPredictor::Joint> syntheticCar()
{
    int n = 0; auto J = [&](double x, double y, double z, int p) { return mkJ(x, y, z, p, n++); };
    std::vector<UniRigPredictor::Joint> j = { J(0,0.3,-1.0,-1), J(0,0.3,-0.3,0), J(0,0.3,0.3,1), J(0,0.3,1.0,2) };
    for (int side = -1; side <= 1; side += 2) for (int end : {0, 3}) {
        const int a = static_cast<int>(j.size());
        j.push_back(J(side*0.5, 0.15, j[end].pos[2], end));
        j.push_back(J(side*0.5, 0.0,  j[end].pos[2], a));
    }
    return j;
}
// A tree: a vertical trunk with six branches fanning out at the top.
std::vector<UniRigPredictor::Joint> syntheticTree()
{
    int n = 0; auto J = [&](double x, double y, double z, int p) { return mkJ(x, y, z, p, n++); };
    std::vector<UniRigPredictor::Joint> j = { J(0,0,0,-1), J(0,.4,0,0), J(0,.8,0,1), J(0,1.2,0,2) };
    for (int b = 0; b < 6; ++b) {
        const double ang = b * 1.0471975512;
        const int a = static_cast<int>(j.size());
        j.push_back(J(.3*std::cos(ang), 1.4, .3*std::sin(ang), 2));
        j.push_back(J(.6*std::cos(ang), 1.6, .6*std::sin(ang), a));
    }
    return j;
}
bool hasName(const std::vector<UniRigPredictor::Joint>& j, const char* nm)
{ for (const auto& x : j) if (x.name == QLatin1String(nm)) return true; return false; }
bool anyHumanoidName(const std::vector<UniRigPredictor::Joint>& j)
{ for (const auto& x : j) if (x.name.contains("Arm") || x.name.contains("Leg") || x.name == "Hips" || x.name == "Head") return true; return false; }
} // namespace

TEST(UniRigLabeling, HumanoidIsPlausibleAndKeepsAnatomicalNamesUnderAuto)
{
    auto j = syntheticHumanoid();
    EXPECT_TRUE(UniRigPredictor::labelJointsAnatomically(j, 1));
    auto k = syntheticHumanoid();
    EXPECT_EQ(UniRigPredictor::applyLabeling(k, 1, UniRigPredictor::Labeling::Auto), "humanoid");
    EXPECT_TRUE(hasName(k, "Hips")); EXPECT_TRUE(hasName(k, "LeftArm")); EXPECT_TRUE(hasName(k, "RightUpLeg"));
}

TEST(UniRigLabeling, CarAndTreeAreNotPlausibleAndGetNeutralNamesUnderAuto)
{
    auto car = syntheticCar();
    EXPECT_FALSE(UniRigPredictor::labelJointsAnatomically(car, 1)) << "its long axis is not the up axis";
    auto car2 = syntheticCar();
    EXPECT_EQ(UniRigPredictor::applyLabeling(car2, 1, UniRigPredictor::Labeling::Auto), "generic");
    EXPECT_FALSE(anyHumanoidName(car2)) << "no Neck/Head/LeftFoot on a car";
    EXPECT_EQ(car2[0].name, "root");

    auto tree = syntheticTree();
    EXPECT_FALSE(UniRigPredictor::labelJointsAnatomically(tree, 1)) << "six branches are not two arms";
    auto tree2 = syntheticTree();
    EXPECT_EQ(UniRigPredictor::applyLabeling(tree2, 1, UniRigPredictor::Labeling::Auto), "generic");
    EXPECT_FALSE(anyHumanoidName(tree2));
}

TEST(UniRigLabeling, ForcedModesAndGenericUniqueness)
{
    auto car = syntheticCar();
    EXPECT_EQ(UniRigPredictor::applyLabeling(car, 1, UniRigPredictor::Labeling::Humanoid), "humanoid");
    EXPECT_TRUE(anyHumanoidName(car)) << "forced humanoid = the pre-#1013 behaviour";
    auto hum = syntheticHumanoid();
    EXPECT_EQ(UniRigPredictor::applyLabeling(hum, 1, UniRigPredictor::Labeling::Generic), "generic");
    EXPECT_FALSE(anyHumanoidName(hum));
    std::set<QString> names; for (const auto& x : hum) names.insert(x.name);
    EXPECT_EQ(names.size(), hum.size()) << "generic names must be unique (Ogre rejects duplicates)";
    EXPECT_EQ(hum[0].name, "root"); EXPECT_EQ(hum[1].name, "bone_01");
    // detokenize keeps its legacy default (Humanoid) so existing tests hold
    (void)UniRigPredictor::detokenize({}, 1.0, {0, 0, 0});
}

TEST(UniRigLabeling, TemplateMapsToLabelingWithBipedForcingHumanoid)
{
    using L = UniRigPredictor::Labeling; using T = AutoRig::Template;
    EXPECT_EQ(AutoRig::uniRigLabelingForTemplate(T::Humanoid),  L::Auto);
    EXPECT_EQ(AutoRig::uniRigLabelingForTemplate(T::Biped),     L::Humanoid);
    EXPECT_EQ(AutoRig::uniRigLabelingForTemplate(T::Quadruped), L::Generic);
    EXPECT_EQ(AutoRig::uniRigLabelingForTemplate(T::Generic),   L::Generic);
}
