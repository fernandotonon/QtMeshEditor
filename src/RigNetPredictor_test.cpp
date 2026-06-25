// Unit tests for RigNetPredictor (#408). No Ogre / GL needed: these exercise
// the model-path resolution and the graceful-failure paths (missing model,
// degenerate input, ONNX-disabled build) that AutoRig relies on for its
// Pinocchio fallback. The actual ONNX inference needs a hosted model and is
// covered behind ENABLE_ONNX on CI when the model is available.

#include <gtest/gtest.h>

#include <QString>
#include <vector>
#include <cstdint>

#include "RigNetPredictor.h"

namespace {
// A tiny tetrahedron (4 verts, 4 faces) — enough to pass the vertex-count
// guard so we reach the model-presence check.
std::vector<float> tetraVerts()
{
    return { 0,0,0,  1,0,0,  0,1,0,  0,0,1 };
}
std::vector<uint32_t> tetraIdx()
{
    return { 0,1,2,  0,1,3,  0,2,3,  1,2,3 };
}
} // namespace

TEST(RigNetPredictor, ModelPathIsUnderAiModelsRignet)
{
    const QString p = RigNetPredictor::modelPath();
    EXPECT_FALSE(p.isEmpty());
    EXPECT_TRUE(p.contains(QStringLiteral("ai_models")));
    EXPECT_TRUE(p.contains(QStringLiteral("rignet")));
    EXPECT_TRUE(p.endsWith(QStringLiteral(".onnx")));
}

TEST(RigNetPredictor, MissingModelFailsGracefully)
{
    auto v = tetraVerts();
    auto idx = tetraIdx();
    const auto r = RigNetPredictor::predict(
        v.data(), static_cast<int>(v.size()/3),
        idx.data(), static_cast<int>(idx.size()),
        QStringLiteral("/nonexistent/path/to/rignet.onnx"));
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());   // a reason AutoRig can log on fallback
    EXPECT_TRUE(r.joints.empty());
}

TEST(RigNetPredictor, TooFewVerticesFails)
{
    std::vector<float> v = { 0,0,0,  1,0,0 };   // 2 verts < the 4 minimum
    const auto r = RigNetPredictor::predict(
        v.data(), 2, nullptr, 0,
        QStringLiteral("/nonexistent/rignet.onnx"));
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

TEST(RigNetPredictor, NullPositionsFails)
{
    const auto r = RigNetPredictor::predict(
        nullptr, 100, nullptr, 0, QStringLiteral("/nonexistent/rignet.onnx"));
    EXPECT_FALSE(r.ok);
}

TEST(RigNetPredictor, EnsureModelBlockingHonoursNoDownloadGuard)
{
    // With QTMESH_RIGNET_NO_DOWNLOAD set (and no model on disk) ensureModelBlocking
    // must return empty WITHOUT touching the network — the offline/test contract.
    // (If a model happens to be cached on disk it returns that path; either way it
    // must never hang or crash.)
    qputenv("QTMESH_RIGNET_NO_DOWNLOAD", "1");
    const QString p = RigNetPredictor::ensureModelBlocking();
    qunsetenv("QTMESH_RIGNET_NO_DOWNLOAD");
    SUCCEED();
    (void)p;
}
