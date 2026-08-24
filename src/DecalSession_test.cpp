/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — DecalSession unit tests (Paint v2 Slice F, issue #549)

Pure-data: exercises the decal quad state machine + the orthographic commit
frame with plain world vectors. No Ogre scene / GL / viewport.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include <gtest/gtest.h>

#include "DecalSession.h"
#include "ProjectionMath.h"

#include <QImage>

#include <OgreVector3.h>

#include <cmath>

namespace {
QImage solid(int s, QColor c) { QImage i(s, s, QImage::Format_RGBA8888); i.fill(c); return i; }
QImage solidWH(int w, int h, QColor c) { QImage i(w, h, QImage::Format_RGBA8888); i.fill(c); return i; }
} // namespace

TEST(DecalSessionTest, BeginPlaceEditCancelTransitions) {
    DecalSession d;
    EXPECT_EQ(d.state(), DecalSession::State::Idle);
    EXPECT_FALSE(d.active());
    d.begin(solid(8, Qt::white));
    EXPECT_EQ(d.state(), DecalSession::State::Placing);
    d.place(Ogre::Vector3(0, 0, 0), Ogre::Vector3(0, 0, 1), Ogre::Vector3::UNIT_Y, 0.5f);
    EXPECT_EQ(d.state(), DecalSession::State::Editing);
    // Placed on the z=0 plane, normal +Z, up +Y → tangentV along +Y, tangentU +X.
    EXPECT_NEAR(d.rect().normal.z, 1.0f, 1e-5f);
    EXPECT_GT(d.rect().tangentV.dotProduct(Ogre::Vector3::UNIT_Y), 0.4f);
    d.cancel();
    EXPECT_EQ(d.state(), DecalSession::State::Idle);
}

TEST(DecalSessionTest, HitTestZones) {
    DecalSession d;
    d.begin(solid(8, Qt::white));
    d.place(Ogre::Vector3::ZERO, Ogre::Vector3(0, 0, 1), Ogre::Vector3::UNIT_Y, 1.0f);
    EXPECT_EQ(d.hitTest(0.0f, 0.0f), DecalSession::Handle::Body);
    EXPECT_EQ(d.hitTest(0.95f, 0.95f), DecalSession::Handle::RotateCorner);
    EXPECT_EQ(d.hitTest(0.95f, 0.0f), DecalSession::Handle::ScaleEdge);
    EXPECT_EQ(d.hitTest(2.0f, 2.0f), DecalSession::Handle::None);
}

TEST(DecalSessionTest, TranslateRotateScaleEdits) {
    DecalSession d;
    d.begin(solid(8, Qt::white));
    d.place(Ogre::Vector3::ZERO, Ogre::Vector3(0, 0, 1), Ogre::Vector3::UNIT_Y, 1.0f);

    d.translate(Ogre::Vector3(2, 3, 0));
    EXPECT_NEAR(d.rect().center.x, 2.0f, 1e-5f);
    EXPECT_NEAR(d.rect().center.y, 3.0f, 1e-5f);

    const float u0 = d.rect().tangentU.length();
    d.scale(2.0f, 0.5f);
    EXPECT_NEAR(d.rect().tangentU.length(), u0 * 2.0f, 1e-4f);

    // Rotate 90° about +Z: tangentU (+X) → +Y.
    const Ogre::Vector3 uBefore = d.rect().tangentU;
    d.rotate(static_cast<float>(M_PI) / 2.0f);
    EXPECT_NEAR(d.rect().tangentU.dotProduct(uBefore), 0.0f, 1e-3f) << "90° rotate ⟂ original";
}

TEST(DecalSessionTest, WorldToRectUvRoundTrip) {
    DecalSession d;
    d.begin(solid(8, Qt::white));
    d.place(Ogre::Vector3(1, 1, 0), Ogre::Vector3(0, 0, 1), Ogre::Vector3::UNIT_Y, 2.0f);
    // The +U +V corner should map to (1,1) in rect-local coords.
    Ogre::Vector3 c[4]; d.corners(c);
    const Ogre::Vector2 uv = d.worldToRectUv(c[2]);   // (+,+)
    EXPECT_NEAR(uv.x, 1.0f, 1e-4f);
    EXPECT_NEAR(uv.y, 1.0f, 1e-4f);
    const Ogre::Vector2 ctr = d.worldToRectUv(d.rect().center);
    EXPECT_NEAR(ctr.x, 0.0f, 1e-4f);
    EXPECT_NEAR(ctr.y, 0.0f, 1e-4f);
}

TEST(DecalSessionTest, BuildCommitOrthoMapsCornersToNdc) {
    DecalSession d;
    d.begin(solid(16, Qt::red));
    d.place(Ogre::Vector3(0.5f, -0.5f, 0.0f), Ogre::Vector3(0, 0, 1),
            Ogre::Vector3::UNIT_Y, 1.5f);
    const auto ci = d.buildCommit(/*softEdge*/0.0f);
    ASSERT_FALSE(ci.source.isNull());

    Ogre::Vector3 c[4]; d.corners(c);
    // Corner (+U,+V) → NDC (1,1) → viewport UV (1,0) (Y flipped). Centre → (0.5,0.5).
    const auto pPlus = ProjectionMath::projectToViewportUV(c[2], ci.view.viewProj);
    EXPECT_FALSE(pPlus.behind);
    EXPECT_NEAR(pPlus.uv.x, 1.0f, 2e-3f);
    EXPECT_NEAR(pPlus.uv.y, 0.0f, 2e-3f);
    const auto pMinus = ProjectionMath::projectToViewportUV(c[0], ci.view.viewProj);
    EXPECT_NEAR(pMinus.uv.x, 0.0f, 2e-3f);
    EXPECT_NEAR(pMinus.uv.y, 1.0f, 2e-3f);
    const auto pCtr = ProjectionMath::projectToViewportUV(d.rect().center, ci.view.viewProj);
    EXPECT_NEAR(pCtr.uv.x, 0.5f, 2e-3f);
    EXPECT_NEAR(pCtr.uv.y, 0.5f, 2e-3f);

    // camDirection points INTO the surface (opposite the outward +Z normal).
    EXPECT_LT(ci.view.camDirection.z, -0.9f);
}

TEST(DecalSessionTest, SoftEdgeFeathersAlpha) {
    DecalSession d;
    d.begin(solid(32, QColor(255, 255, 255, 255)));   // fully opaque
    d.place(Ogre::Vector3::ZERO, Ogre::Vector3(0, 0, 1), Ogre::Vector3::UNIT_Y, 1.0f);
    const auto ci = d.buildCommit(/*softEdge*/0.4f);
    // Border texel alpha reduced; centre stays opaque.
    EXPECT_LT(qAlpha(ci.source.pixel(0, 0)), 128);
    EXPECT_GT(qAlpha(ci.source.pixel(16, 16)), 250);
}

TEST(DecalSessionTest, PlacePreservesImageAspectRatio) {
    // Commit maps the WHOLE image onto the rect, so a 2:1 image must get a 2:1
    // rect — equal U/V extents would stretch it. halfSize is the V extent.
    const float halfSize = 0.5f;

    DecalSession wide;
    wide.begin(solidWH(64, 32, Qt::white));           // aspect 2.0
    wide.place(Ogre::Vector3::ZERO, Ogre::Vector3(0, 0, 1), Ogre::Vector3::UNIT_Y, halfSize);
    EXPECT_NEAR(wide.rect().tangentV.length(), halfSize, 1e-5f);
    EXPECT_NEAR(wide.rect().tangentU.length(), halfSize * 2.0f, 1e-5f);

    DecalSession tall;
    tall.begin(solidWH(32, 64, Qt::white));           // aspect 0.5
    tall.place(Ogre::Vector3::ZERO, Ogre::Vector3(0, 0, 1), Ogre::Vector3::UNIT_Y, halfSize);
    EXPECT_NEAR(tall.rect().tangentV.length(), halfSize, 1e-5f);
    EXPECT_NEAR(tall.rect().tangentU.length(), halfSize * 0.5f, 1e-5f);

    // Square stays square (and a null image must not divide by zero).
    DecalSession square;
    square.begin(solid(32, Qt::white));
    square.place(Ogre::Vector3::ZERO, Ogre::Vector3(0, 0, 1), Ogre::Vector3::UNIT_Y, halfSize);
    EXPECT_NEAR(square.rect().tangentU.length(), halfSize, 1e-5f);
    EXPECT_NEAR(square.rect().tangentV.length(), halfSize, 1e-5f);
}

TEST(DecalSessionTest, CommitUvOrientationMatchesPreviewMapping) {
    // The viewport preview textures the overlay quad with a fixed UV table
    // (see TexturePaintController::refreshDecalOverlay). That table MUST agree
    // with what buildCommit() actually bakes, or the preview would show the
    // artwork flipped relative to the committed result. Pin the convention:
    // project each corner through the commit View and assert the resulting UV.
    DecalSession d;
    d.begin(solid(8, Qt::white));
    d.place(Ogre::Vector3::ZERO, Ogre::Vector3(0, 0, 1), Ogre::Vector3::UNIT_Y, 1.0f);

    const auto ci = d.buildCommit(0.0f);
    Ogre::Vector3 c[4];
    d.corners(c);   // (-,-), (+,-), (+,+), (-,+)

    // Expected UVs, identical to the preview's kUv table.
    const float expect[4][2] = {{0.0f, 1.0f}, {1.0f, 1.0f},
                                {1.0f, 0.0f}, {0.0f, 0.0f}};
    for (int k = 0; k < 4; ++k) {
        const auto p = ProjectionMath::projectToViewportUV(c[k], ci.view.viewProj);
        ASSERT_FALSE(p.behind) << "corner " << k;
        EXPECT_NEAR(p.uv.x, expect[k][0], 1e-4f) << "corner " << k << " u";
        EXPECT_NEAR(p.uv.y, expect[k][1], 1e-4f) << "corner " << k << " v";
    }
}
