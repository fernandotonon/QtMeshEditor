#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// Issue #508: when Assimp's `scene->GetEmbeddedTexture(...)` produces raw
/// bytes for a texture (typical for FBX assets where the textures are
/// inlined as Video.Content blocks — the Rumba Dancing.fbx export case),
/// MaterialProcessor::loadTexture passes them to Ogre via
/// TextureManager::loadImage / loadRawData. Those calls feed the GPU
/// texture but discard the original bytes — they don't live in any
/// resource group's file index, and the texture's getOrigin() ends up
/// empty. The FBX exporter has no path back to the raw bytes, so the
/// re-exported asset references the texture name but emits no
/// Video.Content payload, and downstream loaders that depend on
/// embedded textures (every FBX importer that doesn't share Ogre's
/// resource path) see "missing normal map".
///
/// This module stashes the raw bytes at import time, keyed by texture
/// resource name. The FBX exporter queries it before falling through to
/// the filesystem-scan path. Process-wide; cleared when no longer needed
/// by Cache::clear(). Pure-data; no Ogre dependency.
namespace EmbeddedTextureCache {

/// Stash bytes for a texture name. Overwrites any prior entry for the
/// same name (subsequent imports of the same file replace the bytes,
/// matching the TextureManager semantics).
void store(const std::string& textureName, const std::vector<uint8_t>& bytes);

/// Convenience overload that copies `byteCount` bytes from `data`.
/// Lets the caller hand over a raw byte buffer (e.g. an `aiTexel*`
/// from Assimp) by reinterpret_casting *once at the call site* to
/// `const std::byte*` — the idiomatic C++17 type for opaque byte
/// memory. Internally we still store `uint8_t` so retrieve() / the
/// FBX exporter can copy straight into the wire-format buffer.
void store(const std::string& textureName, const std::byte* data, std::size_t byteCount);

/// Retrieve bytes by texture name. Returns an empty vector when nothing
/// was stashed for that name.
std::vector<uint8_t> retrieve(const std::string& textureName);

/// Drop every entry. Call this between unrelated imports if memory
/// pressure matters; otherwise the cache lives as long as the process.
void clear();

} // namespace EmbeddedTextureCache
