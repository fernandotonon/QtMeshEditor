// Audio2Face (#1019) blendshape solve. Pure data — no Ogre, no ONNX, no model
// file — so every rule is pinned without a scene or a 200 MB download.
#include <gtest/gtest.h>

#include "AudioToFace/BlendshapeSolve.h"

#include <cmath>
#include <vector>

using AudioToFace::PoseBasis;
using AudioToFace::PoseDelta;
using AudioToFace::SolveOptions;
using AudioToFace::solveBlendshapeWeights;

namespace {

// Three orthogonal single-axis "shapes" over 4 vertices. Orthogonality makes
// the expected answer exact, so a wrong result cannot hide behind conditioning.
std::vector<PoseDelta> orthoBasis()
{
    const size_t n = 12;   // 4 verts * xyz
    std::vector<PoseDelta> p(3, PoseDelta(n, 0.0f));
    for (int v = 0; v < 4; ++v) {
        p[0][size_t(v)*3 + 0] = 1.0f;   // all X
        p[1][size_t(v)*3 + 1] = 1.0f;   // all Y
        p[2][size_t(v)*3 + 2] = 1.0f;   // all Z
    }
    return p;
}

// Options with the regularisers off, so a test measures the FIT rather than
// the priors. Each penalty is then switched on individually by the test that
// is about it.
SolveOptions bare(size_t n)
{
    SolveOptions o;
    o.l2 = 0.0; o.l1 = 0.0; o.temporal = 0.0; o.symmetry = 0.0;
    o.active.assign(n, 1);
    o.symmetryPartner.assign(n, -1);
    o.maxIterations = 2000;
    o.tolerance = 1e-10;
    return o;
}

std::vector<float> combine(const std::vector<PoseDelta>& p, const std::vector<float>& w)
{
    std::vector<float> out(p.front().size(), 0.0f);
    for (size_t i = 0; i < p.size(); ++i)
        for (size_t k = 0; k < out.size(); ++k)
            out[k] += w[i] * p[i][k];
    return out;
}

}  // namespace

TEST(BlendshapeSolve, RecoversTheWeightsThatProducedTheTarget)
{
    const auto p = orthoBasis();
    const std::vector<float> truth{0.7f, 0.25f, 0.0f};
    const auto target = combine(p, truth);

    const auto r = solveBlendshapeWeights(p, target, {}, bare(3));
    ASSERT_EQ(r.weights.size(), 3u);
    for (size_t i = 0; i < 3; ++i)
        EXPECT_NEAR(r.weights[i], truth[i], 2e-3) << "pose " << i;
    EXPECT_LT(r.residual, 1e-3);
}

// A blendshape weight below 0 is not a pose — it is the shape INVERTED, which
// renders as the face turning inside out. Above 1 it overshoots the authored
// extreme. The solve must respect the box, not clamp afterwards: a clamped
// unconstrained answer is no longer the least-squares fit.
TEST(BlendshapeSolve, WeightsStayInsideTheUnitBoxEvenWhenTheFitWantsOut)
{
    const auto p = orthoBasis();
    // Ask for 3x the authored extreme on one axis and a negative on another.
    std::vector<float> target(12, 0.0f);
    for (int v = 0; v < 4; ++v) {
        target[size_t(v)*3 + 0] =  3.0f;
        target[size_t(v)*3 + 1] = -2.0f;
    }
    const auto r = solveBlendshapeWeights(p, target, {}, bare(3));
    for (float w : r.weights) {
        EXPECT_GE(w, 0.0f);
        EXPECT_LE(w, 1.0f);
        EXPECT_TRUE(std::isfinite(w));
    }
    EXPECT_NEAR(r.weights[0], 1.0f, 1e-3) << "should saturate at the extreme";
    EXPECT_NEAR(r.weights[1], 0.0f, 1e-3) << "a negative fit must floor at 0";
}

// Audio cannot predict brows, blinks or eye-look. If those poses are left
// solvable the optimiser will happily use them to explain mouth motion, and
// the character blinks every time it speaks.
TEST(BlendshapeSolve, InactivePosesStayAtZeroEvenWhenTheyWouldImproveTheFit)
{
    const auto p = orthoBasis();
    const std::vector<float> truth{0.0f, 0.8f, 0.0f};
    const auto target = combine(p, truth);

    auto o = bare(3);
    o.active = {1, 0, 1};      // the pose that actually explains the target is off
    const auto r = solveBlendshapeWeights(p, target, {}, o);

    EXPECT_FLOAT_EQ(r.weights[1], 0.0f) << "a disabled pose must never be used";
    // And the others must not be recruited to fake it: the basis is
    // orthogonal, so no combination of X and Z can produce Y motion.
    EXPECT_NEAR(r.weights[0], 0.0f, 1e-3);
    EXPECT_NEAR(r.weights[2], 0.0f, 1e-3);
    EXPECT_GT(r.residual, 0.5) << "the target is genuinely unreachable; the "
                                  "solver must report that, not hide it";
}

// Speech is near-symmetric, and an unbalanced smile reads as a defect. The
// symmetry term ties mirrored pairs together; NVIDIA ships it at 100.0, two
// orders above the other penalties, which is a deliberate statement that
// asymmetry must be strongly justified by the data.
TEST(BlendshapeSolve, SymmetryPullsMirroredPairsTogether)
{
    const auto p = orthoBasis();
    // Target wants pose 0 strongly and pose 1 not at all.
    std::vector<float> target(12, 0.0f);
    for (int v = 0; v < 4; ++v) target[size_t(v)*3 + 0] = 0.9f;

    auto o = bare(3);
    o.symmetryPartner = {1, 0, -1};    // 0 and 1 are mirrors of each other

    const auto free_ = solveBlendshapeWeights(p, target, {}, o);
    const double gapFree = std::abs(double(free_.weights[0]) - double(free_.weights[1]));

    o.symmetry = 100.0;
    const auto tied = solveBlendshapeWeights(p, target, {}, o);
    const double gapTied = std::abs(double(tied.weights[0]) - double(tied.weights[1]));

    EXPECT_LT(gapTied, gapFree) << "symmetry must close the gap between mirrors";
    EXPECT_LT(gapTied, 0.05) << "at strength 100 the pair should be near-equal";
}

// Without a temporal term consecutive frames solve independently, and the
// small frame-to-frame ambiguity of an over-complete basis shows up as jitter.
TEST(BlendshapeSolve, TemporalTermHoldsTheSolveNearThePreviousFrame)
{
    const auto p = orthoBasis();
    const std::vector<float> prev{0.9f, 0.0f, 0.0f};
    // The new frame wants something quite different.
    std::vector<float> target(12, 0.0f);
    for (int v = 0; v < 4; ++v) target[size_t(v)*3 + 0] = 0.1f;

    auto o = bare(3);
    const auto jumpy = solveBlendshapeWeights(p, target, prev, o);
    o.temporal = 50.0;
    const auto held = solveBlendshapeWeights(p, target, prev, o);

    EXPECT_GT(held.weights[0], jumpy.weights[0])
        << "a strong temporal term must resist the jump away from 0.9";
}

// An empty or mismatched input must yield a NEUTRAL face, not an error and
// not garbage: a silent passage in a take is normal, and the rig should simply
// rest.
TEST(BlendshapeSolve, DegenerateInputYieldsNeutralRatherThanFailing)
{
    const auto p = orthoBasis();
    auto o = bare(3);

    const auto empty = solveBlendshapeWeights({}, {}, {}, o);
    EXPECT_TRUE(empty.weights.empty());

    // Wrong-sized target for the basis.
    const auto mismatch = solveBlendshapeWeights(p, std::vector<float>(5, 1.0f), {}, o);
    ASSERT_EQ(mismatch.weights.size(), 3u);
    for (float w : mismatch.weights) EXPECT_FLOAT_EQ(w, 0.0f);

    // All-zero target (silence) fits perfectly with all-zero weights.
    const auto silent = solveBlendshapeWeights(p, std::vector<float>(12, 0.0f), {}, o);
    for (float w : silent.weights) EXPECT_NEAR(w, 0.0f, 1e-4);
}

// The basis is fixed for a whole take but the target changes every frame, so
// the Gram matrix is built once. Reusing it must give the same answer as
// solving from scratch — otherwise the optimisation silently changes results.
TEST(BlendshapeSolve, PrebuiltBasisMatchesTheOneShotSolve)
{
    const auto p = orthoBasis();
    const std::vector<float> truth{0.4f, 0.6f, 0.2f};
    const auto target = combine(p, truth);
    auto o = bare(3);

    PoseBasis basis;
    basis.build(p);
    ASSERT_TRUE(basis.valid());
    EXPECT_EQ(basis.poseCount(), 3u);
    EXPECT_EQ(basis.valueCount(), 12u);

    const auto viaBasis = basis.solve(target, {}, o);
    const auto oneShot  = solveBlendshapeWeights(p, target, {}, o);
    ASSERT_EQ(viaBasis.weights.size(), oneShot.weights.size());
    for (size_t i = 0; i < viaBasis.weights.size(); ++i)
        EXPECT_NEAR(viaBasis.weights[i], oneShot.weights[i], 1e-6) << "pose " << i;
}

// A ragged basis means the caller mixed characters or mis-parsed the archive.
// Fitting against the shortest would produce plausible-looking nonsense.
TEST(BlendshapeSolve, RaggedBasisIsRejectedRatherThanTruncated)
{
    std::vector<PoseDelta> p{PoseDelta(12, 1.0f), PoseDelta(9, 1.0f)};
    PoseBasis basis;
    basis.build(p);
    EXPECT_FALSE(basis.valid()) << "poses describing different meshes must not fit";
}

// Multipliers and offsets are per-character trims from NVIDIA's config; they
// are applied to the OUTPUT, and must not push a weight outside the box.
TEST(BlendshapeSolve, MultipliersAndOffsetsAreAppliedAndStillClamped)
{
    const auto p = orthoBasis();
    const std::vector<float> truth{0.5f, 0.0f, 0.0f};
    const auto target = combine(p, truth);

    auto o = bare(3);
    o.multipliers = {4.0f, 1.0f, 1.0f};   // 0.5 * 4 = 2.0 -> must clamp to 1
    o.offsets     = {0.0f, -1.0f, 0.0f};  // negative offset -> must floor at 0
    const auto r = solveBlendshapeWeights(p, target, {}, o);

    EXPECT_FLOAT_EQ(r.weights[0], 1.0f);
    EXPECT_FLOAT_EQ(r.weights[1], 0.0f);
}

// A built basis owns its poses, so a COPY (or move) must keep working after
// the original is gone. This used to hold a pointer at its own member vector,
// which a copy left aimed into the source object — fine until the source died.
TEST(BlendshapeSolve, ACopiedBasisOutlivesTheOriginal)
{
    const auto p = orthoBasis();
    const std::vector<float> truth{0.5f, 0.0f, 0.0f};
    const auto target = combine(p, truth);

    std::vector<float> expected;
    PoseBasis copy;
    {
        PoseBasis original;
        original.build(p);
        ASSERT_TRUE(original.valid());
        expected = original.solve(target, {}, bare(3)).weights;
        copy = original;                       // copy, then let `original` die
    }
    ASSERT_TRUE(copy.valid());
    const auto got = copy.solve(target, {}, bare(3)).weights;
    ASSERT_EQ(got.size(), expected.size());
    for (size_t i = 0; i < got.size(); ++i)
        EXPECT_FLOAT_EQ(got[i], expected[i]) << "weight " << i;
}

// A FRACTIONAL multiplier is how the pipeline corrects a counter-shape whose
// convention differs between the solving basis and the rig being driven --
// `mouthClose` is the real case (#1019). Two properties matter and neither is
// covered by the clamping test above: the weight must scale PROPORTIONALLY
// (not merely change), and the residual must be untouched, because the trim is
// applied after the fit and must not be mistaken for a better or worse solve.
TEST(BlendshapeSolve, FractionalMultiplierScalesWeightWithoutChangingTheFit)
{
    const auto p = orthoBasis();
    const std::vector<float> truth{0.6f, 0.0f, 0.0f};
    const auto target = combine(p, truth);

    const auto plain = solveBlendshapeWeights(p, target, {}, bare(3));
    ASSERT_GT(plain.weights[0], 0.2f) << "the un-trimmed solve must find a real weight";

    auto o = bare(3);
    o.multipliers = {0.5f, 1.0f, 1.0f};
    const auto trimmed = solveBlendshapeWeights(p, target, {}, o);

    EXPECT_NEAR(trimmed.weights[0], plain.weights[0] * 0.5f, 1e-6)
        << "a 0.5 multiplier must halve the emitted weight";
    EXPECT_NEAR(trimmed.residual, plain.residual, 1e-9)
        << "the trim is applied after the fit, so it must not move the residual";
}
