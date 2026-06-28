#include "UvSeamData.h"

#include <algorithm>

namespace UvSeamData {

namespace {

constexpr const char* kSeamsKeyPrefix = "qtme.seams.";
constexpr const char* kPinsKeyPrefix = "qtme.uv_pins.";

using EdgeKeyList = std::vector<uint64_t>;
using PinList = std::vector<uint32_t>;

std::string seamsKey(size_t subMeshIndex)
{
    return std::string(kSeamsKeyPrefix) + std::to_string(subMeshIndex);
}

std::string pinsKey(size_t subMeshIndex)
{
    return std::string(kPinsKeyPrefix) + std::to_string(subMeshIndex);
}

} // namespace

EdgeKey makeEdgeKey(unsigned int a, unsigned int b)
{
    if (a > b)
        std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b);
}

bool globalVertToSubLocal(const EditableMesh& mesh, int globalVert,
                          size_t& subMeshIndex, unsigned int& localVert)
{
    if (globalVert < 0)
        return false;
    int offset = 0;
    for (size_t si = 0; si < mesh.subMeshes().size(); ++si) {
        const int count = static_cast<int>(mesh.subMeshes()[si].vertices.size());
        if (globalVert < offset + count) {
            subMeshIndex = si;
            localVert = static_cast<unsigned int>(globalVert - offset);
            return true;
        }
        offset += count;
    }
    return false;
}

EdgeKey globalEdgeToLocalKey(const EditableMesh& mesh, int gv0, int gv1,
                             size_t& subMeshIndex, EdgeKey& localKey)
{
    size_t sub0 = 0, sub1 = 0;
    unsigned int lv0 = 0, lv1 = 0;
    if (!globalVertToSubLocal(mesh, gv0, sub0, lv0)
        || !globalVertToSubLocal(mesh, gv1, sub1, lv1)
        || sub0 != sub1) {
        subMeshIndex = 0;
        localKey = 0;
        return 0;
    }
    subMeshIndex = sub0;
    localKey = makeEdgeKey(lv0, lv1);
    return localKey;
}

bool isSeam(const EditableSubMesh& sub, unsigned int a, unsigned int b)
{
    return sub.seamEdges.count(makeEdgeKey(a, b)) != 0;
}

void setSeam(EditableSubMesh& sub, unsigned int a, unsigned int b, bool seam)
{
    const EdgeKey key = makeEdgeKey(a, b);
    if (seam)
        sub.seamEdges.insert(key);
    else
        sub.seamEdges.erase(key);
}

bool isPinned(const EditableSubMesh& sub, unsigned int localVert)
{
    return sub.pinnedVertices.count(localVert) != 0;
}

void setPinned(EditableSubMesh& sub, unsigned int localVert, bool pinned)
{
    if (pinned)
        sub.pinnedVertices.insert(localVert);
    else
        sub.pinnedVertices.erase(localVert);
}

void writeBindingsToMesh(Ogre::Mesh* mesh, const std::vector<EditableSubMesh>& subMeshes)
{
    if (!mesh)
        return;
    auto& bindings = mesh->getUserObjectBindings();
    for (size_t i = 0; i < subMeshes.size(); ++i) {
        const auto& sub = subMeshes[i];
        const std::string seamKey = seamsKey(i);
        const std::string pinKey = pinsKey(i);

        if (sub.seamEdges.empty()) {
            bindings.eraseUserAny(seamKey);
        } else {
            EdgeKeyList payload(sub.seamEdges.begin(), sub.seamEdges.end());
            bindings.setUserAny(seamKey, Ogre::Any(std::move(payload)));
        }

        if (sub.pinnedVertices.empty()) {
            bindings.eraseUserAny(pinKey);
        } else {
            PinList payload(sub.pinnedVertices.begin(), sub.pinnedVertices.end());
            bindings.setUserAny(pinKey, Ogre::Any(std::move(payload)));
        }
    }
}

void readBindingsFromMesh(const Ogre::Mesh* mesh, std::vector<EditableSubMesh>& subMeshes)
{
    if (!mesh)
        return;
    for (size_t i = 0; i < subMeshes.size(); ++i) {
        subMeshes[i].seamEdges.clear();
        subMeshes[i].pinnedVertices.clear();
        const auto vertexCount = static_cast<unsigned int>(subMeshes[i].vertices.size());

        const auto seamAny = mesh->getUserObjectBindings().getUserAny(seamsKey(i));
        if (seamAny.has_value()) {
            try {
                const auto keys = Ogre::any_cast<EdgeKeyList>(seamAny);
                for (const auto key : keys) {
                    const auto a = static_cast<unsigned int>(key >> 32);
                    const auto b = static_cast<unsigned int>(key & 0xffffffffu);
                    if (a < vertexCount && b < vertexCount && a != b)
                        subMeshes[i].seamEdges.insert(key);
                }
            } catch (...) {
            }
        }

        const auto pinAny = mesh->getUserObjectBindings().getUserAny(pinsKey(i));
        if (pinAny.has_value()) {
            try {
                const auto pins = Ogre::any_cast<PinList>(pinAny);
                for (const auto pin : pins) {
                    if (pin < vertexCount)
                        subMeshes[i].pinnedVertices.insert(pin);
                }
            } catch (...) {
            }
        }
    }
}

} // namespace UvSeamData
