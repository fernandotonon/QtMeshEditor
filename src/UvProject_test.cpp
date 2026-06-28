#include <gtest/gtest.h>

#include "EditableMesh.h"
#include "UvProject.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

EditableSubMesh makeSubMesh(std::vector<EditableVertex> verts,
                            std::vector<EditableTriangle> tris)
{
    EditableSubMesh sub;
    sub.vertices = std::move(verts);
    sub.triangles = std::move(tris);
    return sub;
}

EditableMesh xyQuadMesh(float size = 1.f)
{
    const float h = size * 0.5f;
    EditableMesh mesh;
    mesh.subMeshes().push_back(makeSubMesh(
        {
            EditableVertex{.position = {-h, -h, 0.f}},
            EditableVertex{.position = { h, -h, 0.f}},
            EditableVertex{.position = { h,  h, 0.f}},
            EditableVertex{.position = {-h,  h, 0.f}},
        },
        {
            EditableTriangle{.indices = {0, 1, 2}},
            EditableTriangle{.indices = {0, 2, 3}},
        }));
    return mesh;
}

EditableMesh cylinderStripMesh(int segments = 16, float height = 2.f)
{
    std::vector<EditableVertex> verts;
    verts.reserve(static_cast<size_t>(segments) * 2);
    for (int ring = 0; ring < 2; ++ring) {
        const float y = ring == 0 ? 0.f : height;
        for (int i = 0; i < segments; ++i) {
            const float ang = static_cast<float>(i) / segments * 2.f * static_cast<float>(M_PI);
            verts.push_back(EditableVertex{
                .position = {std::cos(ang), y, std::sin(ang)},
            });
        }
    }

    std::vector<EditableTriangle> tris;
    for (int i = 0; i < segments; ++i) {
        const unsigned int i0 = static_cast<unsigned int>(i);
        const unsigned int i1 = static_cast<unsigned int>((i + 1) % segments);
        const unsigned int j0 = static_cast<unsigned int>(i + segments);
        const unsigned int j1 = static_cast<unsigned int>(((i + 1) % segments) + segments);
        tris.push_back(EditableTriangle{.indices = {i0, j0, i1}});
        tris.push_back(EditableTriangle{.indices = {i1, j0, j1}});
    }

    EditableMesh mesh;
    mesh.subMeshes().push_back(makeSubMesh(std::move(verts), std::move(tris)));
    return mesh;
}

UvProject::Selection allTriangles(const EditableMesh& mesh)
{
    UvProject::Selection selection;
    selection.includeTriangle.resize(mesh.subMeshes().size());
    return selection;
}

bool uvsInUnitRange(const EditableMesh& mesh)
{
    for (const auto& sub : mesh.subMeshes()) {
        for (const auto& v : sub.vertices) {
            if (!v.hasUV)
                return false;
            if (v.uv.x < -1e-4f || v.uv.x > 1.f + 1e-4f || v.uv.y < -1e-4f || v.uv.y > 1.f + 1e-4f)
                return false;
        }
    }
    return true;
}

} // namespace

TEST(UvProjectTest, BoxProjectNormalizesToUnitRange)
{
    EditableMesh mesh = xyQuadMesh();

    UvProject::Options opts;
    opts.mode = UvProject::Mode::Box;
    opts.boxScale = 1.f;

    const auto report = UvProject::project(mesh, allTriangles(mesh), opts);
    ASSERT_TRUE(report.applied);
    EXPECT_GT(report.vertsChanged, 0);
    EXPECT_TRUE(uvsInUnitRange(mesh));
}

TEST(UvProjectTest, CylinderProjectWrapsAndSpansHeight)
{
    EditableMesh mesh = cylinderStripMesh();

    UvProject::Options opts;
    opts.mode = UvProject::Mode::Cylinder;
    opts.axis = 1;
    opts.boxScale = 1.f;

    const auto report = UvProject::project(mesh, allTriangles(mesh), opts);
    ASSERT_TRUE(report.applied);

    float minU = 1.f, maxU = 0.f, minV = 1.f, maxV = 0.f;
    for (const auto& v : mesh.subMeshes()[0].vertices) {
        ASSERT_TRUE(v.hasUV);
        minU = std::min(minU, v.uv.x);
        maxU = std::max(maxU, v.uv.x);
        minV = std::min(minV, v.uv.y);
        maxV = std::max(maxV, v.uv.y);
    }
    EXPECT_GT(maxU - minU, 0.4f);
    EXPECT_GT(maxV - minV, 0.8f);
}

TEST(UvProjectTest, ViewProjectRequiresCameraMatrices)
{
    EditableMesh mesh = xyQuadMesh();

    UvProject::Options opts;
    opts.mode = UvProject::Mode::View;

    const auto report = UvProject::project(mesh, allTriangles(mesh), opts);
    EXPECT_FALSE(report.applied);
    EXPECT_FALSE(report.error.isEmpty());
}

TEST(UvProjectTest, ResetBoxFillsUnitSquare)
{
    EditableMesh mesh = xyQuadMesh(2.f);

    UvProject::Options opts;
    opts.mode = UvProject::Mode::ResetBox;

    const auto report = UvProject::project(mesh, allTriangles(mesh), opts);
    ASSERT_TRUE(report.applied);
    EXPECT_TRUE(uvsInUnitRange(mesh));
}
