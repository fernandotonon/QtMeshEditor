/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — DerivedMapCache unit tests (Paint v2 Slice G, issue #550)

Exercises the round-trip, the content-hash invalidation the issue's proposed
"revision counter" was replaced by, and the malformed-key path guard.
Writes under the test process's own AppData location; no Ogre scene / GL.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include <gtest/gtest.h>

#include "DerivedMapCache.h"
#include "EditableMesh.h"

#include <OgreVector2.h>
#include <OgreVector3.h>

#include <QDir>
#include <QFile>
#include <QStandardPaths>

namespace {

EditableMesh triMesh(float z = 0.0f)
{
    EditableMesh m;
    m.subMeshes().resize(1);
    EditableSubMesh& sm = m.subMeshes()[0];
    auto push = [&](float x, float y, float u, float v) {
        EditableVertex ev;
        ev.position = Ogre::Vector3(x, y, z);
        ev.normal = Ogre::Vector3(0, 0, 1); ev.hasNormal = true;
        ev.uv = Ogre::Vector2(u, v);        ev.hasUV = true;
        sm.vertices.push_back(ev);
    };
    push(0, 0, 0, 0); push(1, 0, 1, 0); push(0, 1, 0, 1);
    sm.triangles.push_back({0, 1, 2});
    return m;
}

DerivedMap makeMap(int w, int h, float fill)
{
    DerivedMap m;
    m.width = w; m.height = h;
    m.values.assign(static_cast<size_t>(w) * h, fill);
    m.coverage.assign(static_cast<size_t>(w) * h, 1u);
    return m;
}

} // namespace

// The cache writes under <AppData>, which is the USER's real data directory.
// QStandardPaths test mode redirects it to a throwaway location (the same guard
// BrushAssetLibrary_test / GamificationManager_test use), so running the suite
// never pollutes — or reads stale entries from — a real install.
class DerivedMapCacheTest : public ::testing::Test
{
protected:
    void SetUp() override { QStandardPaths::setTestModeEnabled(true); }
    void TearDown() override
    {
        const QString root = DerivedMapCache::cacheRootDirectory();
        if (!root.isEmpty()) QDir(root).removeRecursively();
        QStandardPaths::setTestModeEnabled(false);
    }
};

TEST_F(DerivedMapCacheTest, MeshHashIsStableAndGeometrySensitive) {
    const QString a = DerivedMapCache::meshHash(triMesh(0.0f));
    const QString b = DerivedMapCache::meshHash(triMesh(0.0f));
    const QString c = DerivedMapCache::meshHash(triMesh(5.0f));

    EXPECT_EQ(a.size(), 40) << "must be SHA-1 hex so the path guard accepts it";
    EXPECT_EQ(a, b) << "same geometry must hash identically (else every load misses)";
    EXPECT_NE(a, c) << "moved geometry must hash differently — this IS the invalidation";
}

TEST_F(DerivedMapCacheTest, HashIgnoresNonGeometricAttributes) {
    // Vertex colour cannot change cavity/curvature/AO, so it must not force a
    // rebake. If this ever starts failing, painting a vertex colour would
    // silently invalidate every derived map for that mesh.
    EditableMesh m1 = triMesh();
    EditableMesh m2 = triMesh();
    m2.subMeshes()[0].vertices[0].color = Ogre::ColourValue(1, 0, 0, 1);
    m2.subMeshes()[0].vertices[0].hasColor = true;
    EXPECT_EQ(DerivedMapCache::meshHash(m1), DerivedMapCache::meshHash(m2));
}

TEST_F(DerivedMapCacheTest, HashChangesWithUvAndNormals) {
    // UV and normals DO affect the output (UV decides where texels land,
    // normals decide the concavity sign), so both must be in the hash.
    EditableMesh uvChanged = triMesh();
    uvChanged.subMeshes()[0].vertices[1].uv = Ogre::Vector2(0.5f, 0.5f);
    EXPECT_NE(DerivedMapCache::meshHash(triMesh()), DerivedMapCache::meshHash(uvChanged));

    EditableMesh nChanged = triMesh();
    nChanged.subMeshes()[0].vertices[1].normal = Ogre::Vector3(1, 0, 0);
    EXPECT_NE(DerivedMapCache::meshHash(triMesh()), DerivedMapCache::meshHash(nChanged));
}

TEST_F(DerivedMapCacheTest, SaveLoadRoundTripsExactly) {
    const QString key = DerivedMapCache::meshHash(triMesh());
    DerivedMapCache::invalidateAll(key);

    const DerivedMap in = makeMap(8, 4, 0.375f);
    QString err;
    ASSERT_TRUE(DerivedMapCache::save(key, DerivedMapKind::Cavity, in, err)) << err.toStdString();
    EXPECT_TRUE(DerivedMapCache::has(key, DerivedMapKind::Cavity));

    DerivedMap out;
    ASSERT_TRUE(DerivedMapCache::load(key, DerivedMapKind::Cavity, out, err)) << err.toStdString();
    EXPECT_EQ(out.width, in.width);
    EXPECT_EQ(out.height, in.height);
    ASSERT_EQ(out.values.size(), in.values.size());
    for (size_t i = 0; i < in.values.size(); ++i) EXPECT_FLOAT_EQ(out.values[i], in.values[i]);
    EXPECT_EQ(out.coverage, in.coverage);

    DerivedMapCache::invalidateAll(key);
}

TEST_F(DerivedMapCacheTest, KindsAreStoredSeparately) {
    const QString key = DerivedMapCache::meshHash(triMesh());
    DerivedMapCache::invalidateAll(key);
    QString err;
    ASSERT_TRUE(DerivedMapCache::save(key, DerivedMapKind::Cavity, makeMap(4, 4, 0.1f), err));
    ASSERT_TRUE(DerivedMapCache::save(key, DerivedMapKind::Curvature, makeMap(4, 4, 0.9f), err));

    DerivedMap cav, curv;
    ASSERT_TRUE(DerivedMapCache::load(key, DerivedMapKind::Cavity, cav, err));
    ASSERT_TRUE(DerivedMapCache::load(key, DerivedMapKind::Curvature, curv, err));
    EXPECT_NEAR(cav.values[0], 0.1f, 1e-6f);
    EXPECT_NEAR(curv.values[0], 0.9f, 1e-6f) << "one kind must not overwrite another";

    // Per-kind invalidation must not take the sibling with it.
    DerivedMapCache::invalidate(key, DerivedMapKind::Cavity);
    EXPECT_FALSE(DerivedMapCache::has(key, DerivedMapKind::Cavity));
    EXPECT_TRUE(DerivedMapCache::has(key, DerivedMapKind::Curvature));

    DerivedMapCache::invalidateAll(key);
    EXPECT_FALSE(DerivedMapCache::has(key, DerivedMapKind::Curvature));
}

TEST_F(DerivedMapCacheTest, MalformedKeysAreRejectedAsPaths) {
    // The key becomes a path component, so traversal attempts and wrong-length
    // keys must be structurally impossible rather than sanitised.
    for (const char* bad : {"../../etc/passwd", "not-hex-at-all", "", "abc",
                            "/absolute/path", "0123456789012345678901234567890123456789z"}) {
        const QString k = QString::fromLatin1(bad);
        EXPECT_TRUE(DerivedMapCache::entryDirectory(k).isEmpty()) << bad;
        EXPECT_FALSE(DerivedMapCache::has(k, DerivedMapKind::Cavity)) << bad;
        QString err;
        EXPECT_FALSE(DerivedMapCache::save(k, DerivedMapKind::Cavity, makeMap(2, 2, 0.5f), err)) << bad;
        DerivedMap out;
        EXPECT_FALSE(DerivedMapCache::load(k, DerivedMapKind::Cavity, out, err)) << bad;
    }
    // A well-formed 40-char hex key IS accepted.
    EXPECT_FALSE(DerivedMapCache::entryDirectory(
        QStringLiteral("0123456789abcdef0123456789abcdef01234567")).isEmpty());
}

TEST_F(DerivedMapCacheTest, RefusesToCacheEmptyMap) {
    const QString key = DerivedMapCache::meshHash(triMesh());
    QString err;
    // An empty map means the generator failed; caching it would poison every
    // later load with a valid-looking "no signal" result.
    EXPECT_FALSE(DerivedMapCache::save(key, DerivedMapKind::Cavity, DerivedMap{}, err));
    EXPECT_FALSE(err.isEmpty());
}

TEST_F(DerivedMapCacheTest, CorruptEntryIsAMissNotACrash) {
    const QString key = DerivedMapCache::meshHash(triMesh());
    DerivedMapCache::invalidateAll(key);
    QString err;
    ASSERT_TRUE(DerivedMapCache::save(key, DerivedMapKind::Cavity, makeMap(4, 4, 0.5f), err));

    // Truncate the payload behind the cache's back.
    const QString path = QDir(DerivedMapCache::entryDirectory(key)).filePath(QStringLiteral("cavity.bin"));
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::ReadWrite));
        ASSERT_TRUE(f.resize(12));      // header only, no payload
    }
    DerivedMap out;
    EXPECT_FALSE(DerivedMapCache::load(key, DerivedMapKind::Cavity, out, err));
    EXPECT_FALSE(err.isEmpty());

    DerivedMapCache::invalidateAll(key);
}

TEST_F(DerivedMapCacheTest, LoadOfMissingEntryFailsCleanly) {
    const QString key = QStringLiteral("abcdefabcdefabcdefabcdefabcdefabcdefabcd");
    DerivedMapCache::invalidateAll(key);
    DerivedMap out;
    QString err;
    EXPECT_FALSE(DerivedMapCache::load(key, DerivedMapKind::AmbientOcclusion, out, err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_TRUE(out.empty());
}
