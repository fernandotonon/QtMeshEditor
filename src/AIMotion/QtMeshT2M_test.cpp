#include "QtMeshT2M.h"

#include <gtest/gtest.h>

#include <cmath>

using AIMotion::QtMeshT2M;

namespace {

// A clip whose every role (except the never-supervised 14/18) turns about Y by `degPerFrame[t]` between t and t+1.
QtMeshT2M::Frames clipWithSpeeds(const std::vector<float>& degPerFrame)
{
    QtMeshT2M::Frames q;
    float angle = 0.0f;
    for (size_t t = 0; t <= degPerFrame.size(); ++t) {
        const float h = angle * 0.5f * 3.14159265f / 180.0f;
        QtMeshT2M::Pose p(22, {0.0f, std::sin(h), 0.0f, std::cos(h)});
        p[14] = p[18] = {0.0f, 0.0f, 0.0f, 1.0f};
        q.push_back(p);
        if (t < degPerFrame.size()) angle += degPerFrame[t];
    }
    return q;
}

} // namespace

TEST(QtMeshT2M, PlanFramesAddsALeadInMarginWithinTheModelLength)
{
    int request = 0, trim = 0;
    QtMeshT2M::planFrames(60, &request, &trim);
    EXPECT_EQ(request, 60 + QtMeshT2M::kLeadInFrames);
    EXPECT_EQ(trim, QtMeshT2M::kLeadInFrames);

    QtMeshT2M::planFrames(140, &request, &trim);   // the margin shrinks to what the model can still produce
    EXPECT_EQ(request, QtMeshT2M::kMaxFrames);
    EXPECT_EQ(trim, 10);

    QtMeshT2M::planFrames(400, &request, &trim);   // longer than the model: clamped, nothing to trim
    EXPECT_EQ(request, QtMeshT2M::kMaxFrames);
    EXPECT_EQ(trim, 0);

    QtMeshT2M::planFrames(60, &request, &trim, 60);   // a shorter custom export: never ask past its T
    EXPECT_EQ(request, 60);
    EXPECT_EQ(trim, 0);
    QtMeshT2M::planFrames(45, &request, &trim, 60);
    EXPECT_EQ(request, 60);
    EXPECT_EQ(trim, 15);
}

TEST(QtMeshT2M, LeadInTrimStopsAtTheFirstMovingFrame)
{
    std::vector<float> speeds(10, 0.2f);          // still start
    speeds.insert(speeds.end(), 20, 5.0f);         // then moving
    const auto q = clipWithSpeeds(speeds);
    EXPECT_EQ(QtMeshT2M::leadInFrames(q, 30), 10);
    EXPECT_EQ(QtMeshT2M::leadInFrames(q, 4), 4);   // never more than allowed
}

TEST(QtMeshT2M, LeadInTrimKeepsAClipThatMovesFromTheStart)
{
    const auto q = clipWithSpeeds(std::vector<float>(20, 3.0f));
    EXPECT_EQ(QtMeshT2M::leadInFrames(q, 30), 0);
}

TEST(QtMeshT2M, LeadInTrimIgnoresTheUnsupervisedRoles)
{
    // Only roles 14/18 move (the model outputs noise there, pinned to identity on export): still a still start.
    auto q = clipWithSpeeds(std::vector<float>(10, 0.0f));
    for (size_t t = 0; t < q.size(); ++t) {
        const float h = float(t) * 0.3f;
        q[t][14] = q[t][18] = {std::sin(h), 0.0f, 0.0f, std::cos(h)};
    }
    EXPECT_EQ(QtMeshT2M::leadInFrames(q, 30), 10);   // every frame pair is still: trims all but the last frame
}

TEST(QtMeshT2M, ValidateOptionsRejectsWhatTheModelCannotDo)
{
    QtMeshT2M::Options o;
    QString err;
    EXPECT_FALSE(QtMeshT2M::validateOptions(o, &err));   // no prompt
    o.prompt = QStringLiteral("a person walks forward");
    EXPECT_TRUE(QtMeshT2M::validateOptions(o, &err)) << err.toStdString();
    o.prompt = QStringLiteral("walk forward\nsit down");
    EXPECT_FALSE(QtMeshT2M::validateOptions(o, &err));   // multi-segment timelines are Kimodo only
    EXPECT_TRUE(err.contains(QStringLiteral("Kimodo")));
    o.prompt = QStringLiteral("a person jumps");
    o.variations = 17;
    EXPECT_FALSE(QtMeshT2M::validateOptions(o, &err));
    o.variations = 1;
    o.duration = 0.0;
    EXPECT_FALSE(QtMeshT2M::validateOptions(o, &err));
}

TEST(QtMeshT2M, ModelFilesFollowTheBaseUrlOverride)
{
    qputenv("QTMESH_QTMESH_T2M_BASE_URL", "https://example.org/t2m");
    const auto files = QtMeshT2M::modelFiles();
    qunsetenv("QTMESH_QTMESH_T2M_BASE_URL");
    ASSERT_EQ(files.size(), 5u);
    EXPECT_EQ(files[0].url, QStringLiteral("https://example.org/t2m/t2m-text.onnx"));
    for (const auto& f : files) EXPECT_TRUE(f.sha256.isEmpty()) << "an overridden host must not be held to the pinned digests";
}

TEST(QtMeshT2M, GenerateWithoutTheModelFailsCleanly)
{
    qputenv("QTMESH_QTMESH_T2M_NO_DOWNLOAD", "1");
    QtMeshT2M::Options o;
    o.prompt = QStringLiteral("a person walks forward");
    if (QtMeshT2M::modelsInstalled()) GTEST_SUCCEED() << "the model is installed on this machine; nothing to check";
    else {
        const auto r = QtMeshT2M::generate(o);
        EXPECT_FALSE(r.ok);
        EXPECT_FALSE(r.error.isEmpty());
        QString err;
        EXPECT_FALSE(QtMeshT2M::downloadModels(&err));
        EXPECT_TRUE(err.contains(QStringLiteral("NO_DOWNLOAD")));
    }
    qunsetenv("QTMESH_QTMESH_T2M_NO_DOWNLOAD");
}
