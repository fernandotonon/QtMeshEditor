/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — on-disk cache for cavity / curvature / AO maps
(Paint v2 Slice G, issue #550)

Versioned cache under <AppData>/paint/derived_maps/<mesh-hash>/<kind>.bin, so a
map is generated once per mesh rather than on every brush stroke.

Invalidation is by CONTENT HASH, not a revision counter: the issue proposed an
"EditableMesh revision counter", but EditableMesh has no such counter and
exposes a public mutable subMeshes() accessor, so any counter could be bypassed
without incrementing. Hashing the geometry we already have to hash for the
directory name makes changed geometry a natural cache miss, with nothing to keep
in sync. See meshHash().

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#ifndef DERIVEDMAPCACHE_H
#define DERIVEDMAPCACHE_H

#include "DerivedMapGenerator.h"

#include <QString>

#include <cstdint>

class EditableMesh;

namespace DerivedMapCache {

constexpr uint32_t kMagic = 0x444D5031u;   // 'DMP1'
/// Bump to invalidate every cached entry at once — do this whenever a
/// generator's output would change for identical input (algorithm tweak,
/// different remap curve, etc.), since the mesh hash alone would not notice.
constexpr uint32_t kFormatVersion = 1;

/// <AppData>/paint/derived_maps. Empty if AppData is unavailable.
QString cacheRootDirectory();
/// <AppData>/paint/derived_maps/<meshHash>. Empty if the key is malformed.
QString entryDirectory(const QString& meshHash);

/// SHA-1 (40 hex chars) over the geometry that affects a derived map:
/// per-submesh vertex positions, normals, UV0 and triangle indices. Vertex
/// colours / bone weights are deliberately excluded — they cannot change
/// cavity, curvature or AO, so including them would cause needless rebakes.
QString meshHash(const EditableMesh& mesh);

bool has(const QString& meshHash, DerivedMapKind kind);
bool load(const QString& meshHash, DerivedMapKind kind, DerivedMap& out, QString& error);
bool save(const QString& meshHash, DerivedMapKind kind, const DerivedMap& in, QString& error);

/// Remove one kind, or the whole mesh entry ("Recalculate derived maps").
void invalidate(const QString& meshHash, DerivedMapKind kind);
void invalidateAll(const QString& meshHash);

} // namespace DerivedMapCache

#endif // DERIVEDMAPCACHE_H
