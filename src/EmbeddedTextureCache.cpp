#include "EmbeddedTextureCache.h"

#include <cstddef>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace EmbeddedTextureCache {

namespace {
std::mutex& m() { static std::mutex s_m; return s_m; }
std::unordered_map<std::string, std::vector<uint8_t>>& tbl() {
    static std::unordered_map<std::string, std::vector<uint8_t>> s_tbl;
    return s_tbl;
}
} // namespace

void store(const std::string& textureName, const std::vector<uint8_t>& bytes)
{
    if (textureName.empty() || bytes.empty()) return;
    std::lock_guard lock(m());
    tbl()[textureName] = bytes;
}

void store(const std::string& textureName, const std::byte* data, std::size_t byteCount)
{
    if (textureName.empty() || !data || byteCount == 0) return;
    // `std::byte` is the C++17 type for opaque byte memory (Sonar
    // S6045). Storage stays `uint8_t` so downstream consumers
    // (the FBX exporter writes through m_w.writePropertyR(bytes),
    // which takes std::vector<uint8_t>) don't need to change.
    std::vector<uint8_t> bytes(byteCount);
    std::memcpy(bytes.data(), data, byteCount);
    std::lock_guard lock(m());
    tbl()[textureName] = std::move(bytes);
}

std::vector<uint8_t> retrieve(const std::string& textureName)
{
    if (textureName.empty()) return {};
    std::lock_guard lock(m());
    auto it = tbl().find(textureName);
    return it == tbl().end() ? std::vector<uint8_t>{} : it->second;
}

void clear()
{
    std::lock_guard lock(m());
    tbl().clear();
}

} // namespace EmbeddedTextureCache
