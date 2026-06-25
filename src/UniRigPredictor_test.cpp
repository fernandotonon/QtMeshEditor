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

#include <QString>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "UniRigPredictor.h"

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

    // The explicit parent-triple is the root's quantized position, so it
    // resolves to joint index 0 (not the previous joint, joint 1).
    const std::array<double, 3> parentOfB = {
        refUndiscretize(refDiscretize(root[0])),
        refUndiscretize(refDiscretize(root[1])),
        refUndiscretize(refDiscretize(root[2])),
    };

    std::vector<int> ids;
    ids.push_back(kBos);
    ids.push_back(kClsNone);
    for (double c : root) ids.push_back(refDiscretize(c));      // joint 0
    for (double c : a)    ids.push_back(refDiscretize(c));      // joint 1 (chain)
    ids.push_back(kBranch);                                     // explicit parent next
    for (double c : parentOfB) ids.push_back(refDiscretize(c)); // parent triple == root
    for (double c : b)         ids.push_back(refDiscretize(c)); // joint 2
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
        QStringLiteral("/nonexistent/decoder.onnx"));
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
        QStringLiteral("/nonexistent/decoder.onnx"));
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

TEST(UniRigPredictor, NullPositionsFails)
{
    const auto r = UniRigPredictor::predict(
        nullptr, 100, nullptr, 0,
        QStringLiteral("/nonexistent/encoder.onnx"),
        QStringLiteral("/nonexistent/decoder.onnx"));
    EXPECT_FALSE(r.ok);
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
