#include "MeshSegmenter.h"

#include "ModelDownloader.h"
#include "OnnxRuntimeSettings.h"

#include <QByteArray>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <tuple>
#include <numeric>
#include <unordered_map>

#include "AppStorage.h"

#ifdef ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#include <random>
#include <unordered_set>
#endif

namespace {
constexpr const char* kClassifierModelFile = "meshseg_category.onnx";
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/segment/";
constexpr const char* kBaseUrlSettingsKey = "ai/segmentModelBaseUrl";

// Global label vocabulary — order MUST match MeshSegmenter::Part.
const char* const kPartIds[] = {
    "unknown", "head", "torso", "left_arm", "right_arm", "left_leg", "right_leg",
    "trunk", "branch", "foliage", "root", "flower",
    "vehicle_body", "wheel", "window", "wing", "rotor",
    "wall", "roof", "door", "chimney", "foundation",
};
static_assert(sizeof(kPartIds) / sizeof(kPartIds[0])
                  == static_cast<size_t>(MeshSegmenter::Part::Count),
              "kPartIds must cover every MeshSegmenter::Part");

// Per-category tables, indexed by Category (Auto shares the Body row).
// kCategoryChannelMaps: LOCAL model output channel → global Part. Channel
// order MUST match scripts/export-meshseg-onnx.py's per-category label
// constants; `window` is one global label shared by vehicle + building.
struct CategoryInfo {
    const char* name;
    const char* modelFile;
    std::vector<int> channelMap;
};
using P = MeshSegmenter::Part;
const CategoryInfo kCategories[] = {
    { "auto", "meshseg.onnx",
      { int(P::Unknown), int(P::Head), int(P::Torso), int(P::LeftArm),
        int(P::RightArm), int(P::LeftLeg), int(P::RightLeg) } },
    { "body", "meshseg.onnx",
      { int(P::Unknown), int(P::Head), int(P::Torso), int(P::LeftArm),
        int(P::RightArm), int(P::LeftLeg), int(P::RightLeg) } },
    { "vegetation", "meshseg_vegetation.onnx",
      { int(P::Unknown), int(P::Trunk), int(P::Branch), int(P::Foliage),
        int(P::Root), int(P::Flower) } },
    { "vehicle", "meshseg_vehicle.onnx",
      { int(P::Unknown), int(P::VehicleBody), int(P::Wheel), int(P::Window),
        int(P::Wing), int(P::Rotor) } },
    { "building", "meshseg_building.onnx",
      { int(P::Unknown), int(P::Wall), int(P::Roof), int(P::Window),
        int(P::Door), int(P::Chimney), int(P::Foundation) } },
};
static_assert(sizeof(kCategories) / sizeof(kCategories[0])
                  == static_cast<size_t>(MeshSegmenter::Category::CategoryCount),
              "kCategories must cover every MeshSegmenter::Category");

const CategoryInfo& categoryInfo(MeshSegmenter::Category c)
{
    const auto i = static_cast<size_t>(c);
    return kCategories[i < static_cast<size_t>(MeshSegmenter::Category::CategoryCount)
                           ? i : 1 /* Body */];
}

// Strip non-semantic prefixes (lowercased): a "mixamorig[N]:" / namespace
// colon prefix and a leading "def_"/"org-"/"mch-"/"ctrl-" rig prefix. Keeps
// separators intact (delimited-token side detection needs them). A namespace
// like "LeftRig:Spine1" MUST be dropped here, or its "left" leaks into side
// detection and a centre bone loses its torso mapping.
QString stripBonePrefixes(const QString& raw)
{
    QString s = raw.toLower();
    const int colon = s.lastIndexOf(':');           // mixamorig:LeftArm → leftarm
    if (colon >= 0) s = s.mid(colon + 1);
    for (const char* p : {"def-", "def_", "def", "org-", "mch-", "ctrl-"})
        if (s.startsWith(QLatin1String(p))) { s = s.mid(int(qstrlen(p))); break; }
    return s;
}

// Normalise a bone name for matching: strip prefixes, then drop separators so
// "LeftArm" / "L_Arm" / "arm.l" / "DEF-upper_arm.L" compare alike.
QString normaliseBoneName(const QString& raw)
{
    QString s = stripBonePrefixes(raw);
    s.remove(' ').remove('_').remove('-').remove('.');
    return s;
}

// Side from the RAW (lower-cased) bone name using explicit side tokens that
// survive separators — handles `_R_`, `.l`, `-r`, and the very common
// `..._joint_R_1` / `arm_joint_L__4_` style where the side letter sits BETWEEN
// separators and a trailing index (which the normalised-name heuristic misses
// because stripping separators leaves `armjointr1`, ending in a digit). Word
// forms ("left"/"right") always win.
char boneSideFromRaw(const QString& rawLower)
{
    if (rawLower.contains(QLatin1String("left")))  return 'l';
    if (rawLower.contains(QLatin1String("right"))) return 'r';
    // A side letter delimited by a separator (or string end) on at least one
    // side, e.g. "_r_", ".l", "_r1", "armr" → side token, not part of a word.
    auto isSep = [](QChar c) {
        return c == '_' || c == '-' || c == '.' || c == ' ' || c == ':';
    };
    for (int i = 0; i < rawLower.size(); ++i) {
        const QChar c = rawLower[i];
        if (c != 'l' && c != 'r') continue;
        const bool sepBefore = (i == 0) || isSep(rawLower[i - 1]);
        if (!sepBefore) continue;                       // must start a token
        // followed by a separator, a digit, or end-of-string → a side marker
        const bool ok = (i + 1 >= rawLower.size())
                        || isSep(rawLower[i + 1])
                        || rawLower[i + 1].isDigit();
        if (ok) return c.toLatin1();
    }
    return 0;
}

// Side from a normalised name → 'l', 'r', or 0 (centre). Word forms win over
// single-letter affixes.
char boneSide(const QString& n)
{
    if (n.contains("left"))  return 'l';
    if (n.contains("right")) return 'r';
    if (n.startsWith('l') && n.size() > 1) return 'l';
    if (n.startsWith('r') && n.size() > 1) return 'r';
    if (n.endsWith('l')) return 'l';
    if (n.endsWith('r')) return 'r';
    return 0;
}

void applyBoneHints(std::vector<int>& labels, const int* boneProximity)
{
    if (!boneProximity) return;
    const int count = static_cast<int>(MeshSegmenter::Part::Count);
    const int unknown = static_cast<int>(MeshSegmenter::Part::Unknown);
    for (int v = 0; v < static_cast<int>(labels.size()); ++v) {
        const int bp = boneProximity[v];
        if (bp > unknown && bp < count)
            labels[v] = bp;
    }
}

void smoothLabelsByTopology(std::vector<int>& labels,
                            const uint32_t* indices,
                            int indexCount,
                            int iterations)
{
    const int vertexCount = static_cast<int>(labels.size());
    const int partCount = static_cast<int>(MeshSegmenter::Part::Count);
    if (vertexCount <= 0 || !indices || indexCount < 3 || iterations <= 0)
        return;

    std::vector<std::vector<int>> neighbours(vertexCount);
    for (int i = 0; i + 2 < indexCount; i += 3) {
        const int a = static_cast<int>(indices[i]);
        const int b = static_cast<int>(indices[i + 1]);
        const int c = static_cast<int>(indices[i + 2]);
        if (a < 0 || b < 0 || c < 0 || a >= vertexCount || b >= vertexCount || c >= vertexCount)
            continue;
        neighbours[a].push_back(b); neighbours[a].push_back(c);
        neighbours[b].push_back(a); neighbours[b].push_back(c);
        neighbours[c].push_back(a); neighbours[c].push_back(b);
    }
    for (auto& ns : neighbours) {
        std::sort(ns.begin(), ns.end());
        ns.erase(std::unique(ns.begin(), ns.end()), ns.end());
    }

    std::vector<int> next(labels.size());
    for (int it = 0; it < iterations; ++it) {
        next = labels;
        for (int v = 0; v < vertexCount; ++v) {
            std::array<int, static_cast<int>(MeshSegmenter::Part::Count)> votes{};
            const int current = labels[v];
            if (current >= 0 && current < partCount)
                votes[current] += 3;
            for (int n : neighbours[v]) {
                const int label = labels[n];
                if (label >= 0 && label < partCount)
                    ++votes[label];
            }
            int best = current;
            int bestVotes = (current >= 0 && current < partCount) ? votes[current] : -1;
            for (int p = 0; p < partCount; ++p) {
                if (votes[p] > bestVotes) {
                    bestVotes = votes[p];
                    best = p;
                }
            }
            next[v] = best;
        }
        labels.swap(next);
    }
}
} // namespace

MeshSegmenter::Options::Options() = default;

int MeshSegmenter::partCount() { return static_cast<int>(Part::Count); }

QString MeshSegmenter::partName(int p)
{
    if (p < 0 || p >= static_cast<int>(Part::Count)) return QStringLiteral("unknown");
    return QString::fromLatin1(kPartIds[p]);
}
QString MeshSegmenter::partName(Part p) { return partName(static_cast<int>(p)); }

QString MeshSegmenter::categoryName(Category c)
{
    return QString::fromLatin1(categoryInfo(c).name);
}

MeshSegmenter::Category MeshSegmenter::categoryFromName(const QString& name, bool* ok)
{
    const QString n = name.trimmed().toLower();
    for (size_t i = 0; i < static_cast<size_t>(Category::CategoryCount); ++i) {
        if (n == QLatin1String(kCategories[i].name)) {
            if (ok) *ok = true;
            return static_cast<Category>(i);
        }
    }
    if (ok) *ok = !n.isEmpty() ? false : true;   // empty = default Auto, valid
    return Category::Auto;
}

int MeshSegmenter::categoryChannelCount(Category c)
{
    return static_cast<int>(categoryInfo(c).channelMap.size());
}

std::vector<int> MeshSegmenter::categoryChannelMap(Category c)
{
    return categoryInfo(c).channelMap;
}

QString MeshSegmenter::modelFileName(Category c)
{
    return QString::fromLatin1(categoryInfo(c).modelFile);
}

MeshSegmenter::Part MeshSegmenter::partForBoneName(const QString& boneName)
{
    const QString n = normaliseBoneName(boneName);
    if (n.isEmpty()) return Part::Unknown;
    // Prefer explicit side tokens in the PREFIX-STRIPPED name (catches
    // `_R_1`/`.l`/`-r` delimited markers); fall back to the normalised-name
    // heuristic. Strip prefixes first so a namespace like "LeftRig:Spine1"
    // doesn't leak its "left" into the side (which would skip the torso
    // mapping for a centre bone).
    char side = boneSideFromRaw(stripBonePrefixes(boneName));
    if (side == 0) side = boneSide(n);
    auto has = [&](std::initializer_list<const char*> cores) {
        for (const char* c : cores) if (n.contains(QLatin1String(c))) return true;
        return false;
    };

    // HEAD region — incl. non-human cranial parts so a cat's ears/snout follow
    // the head, not "arm". NOTE: the bare substring "ear" collides with
    // "forEARm", so an arm name must NOT be misread as an ear — guard it.
    const bool isArmName = has({"arm", "forearm"});
    if ((has({"head", "skull", "cranium", "jaw", "eye", "eyelid", "brow",
              "nose", "snout", "muzzle", "nostril", "tongue", "teeth", "tooth",
              "lip", "cheek", "horn", "antler", "whisker", "face", "neck"})
         || (has({"ear"}) && !isArmName)))
        return Part::Head;

    // Centre / spine / tail → torso (tail has no dedicated label; torso is the
    // closest body region and keeps it out of the limbs).
    if (has({"spine", "chest", "hips", "pelvis", "abdomen", "lowerback",
             "tail", "spine1", "spine2", "ribcage", "belly", "root",
             "torso", "body", "waist", "sternum", "clavicleroot"}) && side == 0)
        return Part::Torso;

    // Arms (incl. wings, fingers, hands, clavicle/shoulder) by side.
    if (has({"arm", "hand", "wrist", "finger", "thumb", "elbow", "forearm",
             "clavicle", "shoulder", "collar", "wing", "palm", "claw"})) {
        if (side == 'l') return Part::LeftArm;
        if (side == 'r') return Part::RightArm;
        return Part::Torso;                  // a centre "arm"? treat as torso
    }

    // Legs (incl. feet, toes, thigh/shin) by side.
    if (has({"leg", "thigh", "shin", "calf", "knee", "foot", "ankle", "toe",
             "femur", "tibia", "heel", "hoof", "paw", "upleg", "buttock"})) {
        if (side == 'l') return Part::LeftLeg;
        if (side == 'r') return Part::RightLeg;
        return Part::Torso;
    }

    return Part::Unknown;
}

bool MeshSegmenter::isModelBackendAvailable()
{
#ifdef ENABLE_ONNX
    return true;
#else
    return false;
#endif
}

QString MeshSegmenter::modelPath(Category c)
{
    return QDir(AppStorage::aiModelsRoot()).filePath(QStringLiteral("segment/")
                                   + modelFileName(c));
}

bool MeshSegmenter::modelPresent(Category c) { return QFileInfo::exists(modelPath(c)); }

QString MeshSegmenter::classifierModelPath()
{
    return QDir(AppStorage::aiModelsRoot()).filePath(QStringLiteral("segment/")
                                   + QString::fromLatin1(kClassifierModelFile));
}

bool MeshSegmenter::classifierModelPresent()
{
    return QFileInfo::exists(classifierModelPath());
}

// ---------------------------------------------------------------------------
// Pure-data helpers
// ---------------------------------------------------------------------------

int MeshSegmenter::connectedComponents(int vertexCount,
                                       const uint32_t* indices, int indexCount,
                                       std::vector<int>& outIslandId)
{
    outIslandId.assign(std::max(0, vertexCount), -1);
    if (vertexCount <= 0) return 0;

    // Union-Find over vertices joined by shared triangles.
    std::vector<int> parent(vertexCount);
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&](int x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto unite = [&](int a, int b) {
        if (a < 0 || b < 0 || a >= vertexCount || b >= vertexCount) return;
        const int ra = find(a), rb = find(b);
        if (ra != rb) parent[ra] = rb;
    };
    for (int i = 0; i + 2 < indexCount; i += 3) {
        const int a = static_cast<int>(indices[i]);
        const int b = static_cast<int>(indices[i + 1]);
        const int c = static_cast<int>(indices[i + 2]);
        unite(a, b); unite(b, c);
    }

    std::unordered_map<int, int> rootToIsland;
    int n = 0;
    for (int v = 0; v < vertexCount; ++v) {
        const int r = find(v);
        auto it = rootToIsland.find(r);
        if (it == rootToIsland.end()) { rootToIsland[r] = n; outIslandId[v] = n; ++n; }
        else outIslandId[v] = it->second;
    }
    return n;
}

std::vector<int> MeshSegmenter::facesFromVertexLabels(const std::vector<int>& vertexLabels,
                                                      const uint32_t* indices, int indexCount)
{
    std::vector<int> faces;
    faces.reserve(std::max(0, indexCount / 3));
    const int vc = static_cast<int>(vertexLabels.size());
    for (int i = 0; i + 2 < indexCount; i += 3) {
        std::array<int, 3> l{ 0, 0, 0 };
        for (int k = 0; k < 3; ++k) {
            const int v = static_cast<int>(indices[i + k]);
            l[k] = (v >= 0 && v < vc) ? vertexLabels[v] : 0;
        }
        // Majority of the 3 vertex labels; all-distinct (no majority) → lowest.
        int label;
        if (l[0] == l[1] || l[0] == l[2]) label = l[0];   // l[0] in a pair
        else if (l[1] == l[2])            label = l[1];   // l[1]==l[2] pair
        else                              label = std::min({l[0], l[1], l[2]});
        faces.push_back(label);
    }
    return faces;
}

namespace {
// Build face-to-face adjacency over shared EDGES. Two faces are neighbours iff
// they share an edge (an unordered vertex-index pair). Returns adjacency as a
// vector<vector<int>> indexed by face id. O(F) with a hash map over edges.
std::vector<std::vector<int>> buildFaceAdjacency(const uint32_t* indices, int faceCount)
{
    std::vector<std::vector<int>> adj(std::max(0, faceCount));
    // edge key (min,max) → first face that used it; on the second, link them.
    std::unordered_map<uint64_t, int> edgeToFace;
    edgeToFace.reserve(static_cast<size_t>(faceCount) * 3);
    auto edgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
        const uint64_t lo = std::min(a, b), hi = std::max(a, b);
        return (lo << 32) | hi;
    };
    for (int f = 0; f < faceCount; ++f) {
        const uint32_t v0 = indices[f * 3 + 0];
        const uint32_t v1 = indices[f * 3 + 1];
        const uint32_t v2 = indices[f * 3 + 2];
        const uint64_t keys[3] = { edgeKey(v0, v1), edgeKey(v1, v2), edgeKey(v2, v0) };
        for (uint64_t k : keys) {
            auto it = edgeToFace.find(k);
            if (it == edgeToFace.end()) {
                edgeToFace.emplace(k, f);
            } else if (it->second != f) {
                // Link both directions (an edge shared by >2 faces links each
                // to the first-seen — acceptable for the cleanup's purposes).
                adj[f].push_back(it->second);
                adj[it->second].push_back(f);
            }
        }
    }
    return adj;
}
} // namespace

int MeshSegmenter::smoothLabelBoundaries(std::vector<int>& faceLabels,
                                         const uint32_t* indices, int indexCount,
                                         int iterations, int minAgree)
{
    const int faceCount = static_cast<int>(faceLabels.size());
    if (faceCount <= 0 || !indices || indexCount < faceCount * 3 || iterations <= 0)
        return 0;
    if (minAgree < 2) minAgree = 2;

    const std::vector<std::vector<int>> adj = buildFaceAdjacency(indices, faceCount);
    int totalFlipped = 0;

    for (int it = 0; it < iterations; ++it) {
        // Compute every flip against the SAME snapshot so the pass is order-
        // independent (a face and its neighbour can't ping-pong within a pass).
        const std::vector<int> snapshot = faceLabels;
        int flippedThisPass = 0;
        for (int f = 0; f < faceCount; ++f) {
            const int self = snapshot[f];
            // Tally neighbour labels.
            std::unordered_map<int, int> votes;
            int selfNeighbours = 0;
            for (int nb : adj[f]) {
                const int nl = snapshot[nb];
                ++votes[nl];
                if (nl == self) ++selfNeighbours;
            }
            if (votes.empty())
                continue;
            // Best OTHER label among neighbours.
            int bestLabel = self, bestVotes = 0;
            for (const auto& kv : votes) {
                if (kv.first == self) continue;
                if (kv.second > bestVotes ||
                    (kv.second == bestVotes && kv.first < bestLabel)) {
                    bestVotes = kv.second;
                    bestLabel = kv.first;
                }
            }
            // Flip only when a different label holds a strict majority of the
            // face's edge-neighbours AND outnumbers the face's own label there —
            // i.e. this face is a tooth poking into `bestLabel`'s territory.
            if (bestLabel != self && bestVotes >= minAgree && bestVotes > selfNeighbours) {
                faceLabels[f] = bestLabel;
                ++flippedThisPass;
            }
        }
        totalFlipped += flippedThisPass;
        if (flippedThisPass == 0)
            break; // seam is straight
    }
    return totalFlipped;
}

namespace {
struct Vec3 {
    float x = 0, y = 0, z = 0;
    float comp(int a) const { return a == 0 ? x : (a == 1 ? y : z); }
};
inline Vec3 faceCentroid(const float* pos, const uint32_t* idx, int f)
{
    Vec3 c;
    for (int k = 0; k < 3; ++k) {
        const int v = static_cast<int>(idx[f * 3 + k]);
        c.x += pos[v * 3 + 0]; c.y += pos[v * 3 + 1]; c.z += pos[v * 3 + 2];
    }
    c.x /= 3.0f; c.y /= 3.0f; c.z /= 3.0f;
    return c;
}
// Canonical mirror pair for symmetry coupling: returns the paired part label,
// or -1 if the label has no left/right mirror.
int mirrorPartLabel(int label)
{
    using P = MeshSegmenter::Part;
    switch (static_cast<P>(label)) {
        case P::LeftArm:  return static_cast<int>(P::RightArm);
        case P::RightArm: return static_cast<int>(P::LeftArm);
        case P::LeftLeg:  return static_cast<int>(P::RightLeg);
        case P::RightLeg: return static_cast<int>(P::LeftLeg);
        default:          return -1;
    }
}
} // namespace

int MeshSegmenter::planarBoundaryRecut(std::vector<int>& faceLabels,
                                       const float* positions, int vertexCount,
                                       const uint32_t* indices, int indexCount,
                                       float axisSnapDeg, float bandFraction)
{
    const int faceCount = static_cast<int>(faceLabels.size());
    if (faceCount <= 0 || !positions || vertexCount <= 0 ||
        !indices || indexCount < faceCount * 3)
        return 0;

    // Mesh diagonal for the band width.
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (int v = 0; v < vertexCount; ++v)
        for (int a = 0; a < 3; ++a) {
            const float p = positions[v * 3 + a];
            mn[a] = std::min(mn[a], p); mx[a] = std::max(mx[a], p);
        }
    const float diag = std::sqrt((mx[0]-mn[0])*(mx[0]-mn[0]) +
                                 (mx[1]-mn[1])*(mx[1]-mn[1]) +
                                 (mx[2]-mn[2])*(mx[2]-mn[2]));
    if (!(diag > 0.0f))
        return 0;
    const float band = bandFraction * diag;

    const std::vector<std::vector<int>> adj = buildFaceAdjacency(indices, faceCount);

    // Precompute face centroids.
    std::vector<Vec3> centroid(faceCount);
    for (int f = 0; f < faceCount; ++f)
        centroid[f] = faceCentroid(positions, indices, f);

    // Enumerate adjacent label pairs (unordered, ordered so a<b).
    auto pairKey = [](int a, int b) -> uint64_t {
        const uint32_t lo = std::min(a, b), hi = std::max(a, b);
        return (static_cast<uint64_t>(lo) << 32) | hi;
    };
    std::unordered_map<uint64_t, std::pair<int,int>> pairs; // key → (labelA,labelB)
    for (int f = 0; f < faceCount; ++f)
        for (int nb : adj[f]) {
            const int la = faceLabels[f], lb = faceLabels[nb];
            if (la != lb)
                pairs[pairKey(la, lb)] = { std::min(la,lb), std::max(la,lb) };
        }
    if (pairs.empty())
        return 0;

    const float cosSnap = std::cos(axisSnapDeg * 3.14159265358979f / 180.0f);

    // A fitted cut plane for one label pair.
    struct Plane { Vec3 n; float d = 0; int axis = -1; bool ok = false; };
    std::unordered_map<uint64_t, Plane> planeFor;

    for (const auto& kv : pairs) {
        const int A = kv.second.first, B = kv.second.second;
        // Boundary faces: faces of A adjacent to B (and vice versa).
        Vec3 cA, cB; int nA = 0, nB = 0;
        std::vector<int> boundary;
        for (int f = 0; f < faceCount; ++f) {
            const int lf = faceLabels[f];
            if (lf != A && lf != B) continue;
            bool onSeam = false;
            for (int nb : adj[f]) {
                const int ln = faceLabels[nb];
                if ((lf == A && ln == B) || (lf == B && ln == A)) { onSeam = true; break; }
            }
            if (!onSeam) continue;
            boundary.push_back(f);
            if (lf == A) { cA.x += centroid[f].x; cA.y += centroid[f].y; cA.z += centroid[f].z; ++nA; }
            else         { cB.x += centroid[f].x; cB.y += centroid[f].y; cB.z += centroid[f].z; ++nB; }
        }
        if (nA == 0 || nB == 0 || boundary.size() < 6)
            continue; // too little seam to fit a plane
        cA.x /= nA; cA.y /= nA; cA.z /= nA;
        cB.x /= nB; cB.y /= nB; cB.z /= nB;

        // Normal = A→B direction (points from A into B).
        Vec3 n{ cB.x - cA.x, cB.y - cA.y, cB.z - cA.z };
        float len = std::sqrt(n.x*n.x + n.y*n.y + n.z*n.z);
        if (!(len > 1e-6f))
            continue;
        n.x /= len; n.y /= len; n.z /= len;

        // Axis-snap when near a world axis.
        int snapAxis = -1;
        const float comp[3] = { std::fabs(n.x), std::fabs(n.y), std::fabs(n.z) };
        for (int a = 0; a < 3; ++a) {
            if (comp[a] >= cosSnap) {
                const float sign = ((a==0?n.x:a==1?n.y:n.z) >= 0) ? 1.0f : -1.0f;
                n = { 0, 0, 0 };
                (a==0 ? n.x : a==1 ? n.y : n.z) = sign;
                snapAxis = a;
                break;
            }
        }

        // Plane offset d = n·(midpoint of the two boundary centroids).
        Vec3 mid{ (cA.x+cB.x)*0.5f, (cA.y+cB.y)*0.5f, (cA.z+cB.z)*0.5f };
        const float d = n.x*mid.x + n.y*mid.y + n.z*mid.z;
        planeFor[kv.first] = Plane{ n, d, snapAxis, true };
    }

    // Symmetry coupling: mirror-paired parts sharing a boundary with the SAME
    // third part (e.g. LeftLeg-Torso and RightLeg-Torso) should cut at the same
    // offset along a shared snapped axis. Average their `d` when the snap axis
    // matches (mirror is across a different axis, so the cut height agrees).
    for (auto& kvA : planeFor) {
        const int a0 = static_cast<int>(kvA.first >> 32);
        const int a1 = static_cast<int>(kvA.first & 0xffffffff);
        // Identify the limb label (the one with a mirror) and the shared part.
        int limb = mirrorPartLabel(a0) >= 0 ? a0 : (mirrorPartLabel(a1) >= 0 ? a1 : -1);
        if (limb < 0 || !kvA.second.ok || kvA.second.axis < 0) continue;
        const int shared = (limb == a0) ? a1 : a0;
        const int mirror = mirrorPartLabel(limb);
        const uint64_t mirrorKey = (static_cast<uint64_t>(std::min(mirror, shared)) << 32)
                                   | static_cast<uint32_t>(std::max(mirror, shared));
        auto it = planeFor.find(mirrorKey);
        if (it == planeFor.end() || !it->second.ok) continue;
        if (it->second.axis != kvA.second.axis) continue;
        // Average the offsets so both limbs cut level. (Both planes' normals are
        // the same snapped axis; signs may differ, so compare |d| along axis.)
        const float avg = 0.5f * (kvA.second.d + it->second.d);
        kvA.second.d = avg;
        it->second.d = avg;
    }

    // Reassign band faces by side of the plane. For each pair, a face currently
    // A or B within `band` of the seam midpoint plane flips to A if it's on the
    // A side (n·c < d), else B.
    int relabelled = 0;
    for (const auto& kv : pairs) {
        auto pit = planeFor.find(kv.first);
        if (pit == planeFor.end() || !pit->second.ok) continue;
        const int A = kv.second.first, B = kv.second.second;
        const Plane& pl = pit->second;
        for (int f = 0; f < faceCount; ++f) {
            const int lf = faceLabels[f];
            if (lf != A && lf != B) continue;
            const Vec3& c = centroid[f];
            const float sd = pl.n.x*c.x + pl.n.y*c.y + pl.n.z*c.z - pl.d; // signed dist
            if (std::fabs(sd) > band) continue; // outside the seam band
            // n points A→B: negative side = A, positive = B.
            const int want = (sd < 0.0f) ? A : B;
            if (want != lf) { faceLabels[f] = want; ++relabelled; }
        }
    }
    return relabelled;
}

int MeshSegmenter::levelLimbCut(std::vector<int>& faceLabels,
                                const float* positions, int vertexCount,
                                const uint32_t* indices, int indexCount,
                                int limbLeft, int limbRight, int shared, int up,
                                float bandFraction)
{
    const int faceCount = static_cast<int>(faceLabels.size());
    if (faceCount <= 0 || !positions || vertexCount <= 0 ||
        !indices || indexCount < faceCount * 3)
        return 0;
    up = std::clamp(up, 0, 2);
    const int lat = (up == 0) ? 2 : 0; // a lateral axis distinct from up

    // Need both limbs and the shared part actually present, or there's no cut.
    int nL = 0, nR = 0, nS = 0;
    for (int l : faceLabels) {
        if (l == limbLeft) ++nL; else if (l == limbRight) ++nR; else if (l == shared) ++nS;
    }
    if (nL == 0 || nR == 0 || nS == 0)
        return 0;

    (void)bandFraction; // (legacy horizontal-band knob; unused by the mirror path)
    const std::vector<std::vector<int>> adj = buildFaceAdjacency(indices, faceCount);
    std::vector<Vec3> centroid(faceCount);
    for (int f = 0; f < faceCount; ++f)
        centroid[f] = faceCentroid(positions, indices, f);

    // MIRROR-SYMMETRISE the two limbs' cut (legs) rather than forcing a flat
    // horizontal waistline. The model's per-leg boundary has a natural DIAGONAL
    // shape (like the arms) — the only real defect is that the LEFT and RIGHT
    // boundaries disagree, so an explode looks lopsided. We reflect the labelling
    // across the SAGITTAL plane (the lateral centre of the limb region) and make
    // each near-seam face agree with its mirror — preserving the diagonal cut,
    // making the two legs symmetric, and (because reflection maps a foot to the
    // OPPOSITE foot's place with the limb labels swapped) keeping each foot with
    // its own leg. Horizontal height is never used, so the torso skirt bottom is
    // not dragged down (the user's two regressions with the flat cut).

    // Per-limb lateral means (count-INDEPENDENT). The sagittal centre is the
    // MIDPOINT of the two, NOT a mean over all limb faces — a face-count-weighted
    // mean would let the leg with more faces pull the midline toward itself
    // (exactly the asymmetry this pass fixes), flipping near-crotch faces to the
    // wrong leg (CodeRabbit). Also derive which limb is on the +lat side.
    double latLSum = 0, latRSum = 0; long nLf = 0, nRf = 0;
    for (int f = 0; f < faceCount; ++f) {
        const int lf = faceLabels[f];
        if (lf == limbLeft)  { latLSum += centroid[f].comp(lat); ++nLf; }
        else if (lf == limbRight) { latRSum += centroid[f].comp(lat); ++nRf; }
    }
    if (nLf == 0 || nRf == 0) return 0;
    const double latLMean = latLSum / nLf, latRMean = latRSum / nRf;
    const float latCentre = float(0.5 * (latLMean + latRMean));
    const bool rightIsPositive = (latRMean >= latLMean);
    auto limbOnSide = [&](bool positive) { return (positive == rightIsPositive) ? limbRight : limbLeft; };

    // Only mirror-fix faces NEAR the limb↔torso seam (bounded flood from seam
    // faces), so distant geometry (a raised foot, bent knee) is never touched.
    std::vector<int> seamFaces;
    for (int f = 0; f < faceCount; ++f) {
        const int lf = faceLabels[f];
        const bool isLimb = (lf == limbLeft || lf == limbRight);
        if (!isLimb && lf != shared) continue;
        for (int nb : adj[f]) {
            const int ln = faceLabels[nb];
            if ((isLimb && ln == shared) ||
                (lf == shared && (ln == limbLeft || ln == limbRight))) { seamFaces.push_back(f); break; }
        }
    }
    if (seamFaces.size() < 6) return 0;
    const int maxHops = 8;
    std::vector<int> hop(faceCount, -1);
    std::vector<int> frontier = seamFaces;
    for (int s : seamFaces) hop[s] = 0;
    for (int d = 0; d < maxHops && !frontier.empty(); ++d) {
        std::vector<int> next;
        for (int f : frontier)
            for (int nb : adj[f])
                if (hop[nb] == -1) { hop[nb] = d + 1; next.push_back(nb); }
        frontier.swap(next);
    }

    // Spatial hash of face centroids for nearest-mirror lookup (grid keyed by
    // rounded position; search the 27 neighbouring cells).
    float bmin[3] = {1e30f,1e30f,1e30f}, bmax[3] = {-1e30f,-1e30f,-1e30f};
    for (int f = 0; f < faceCount; ++f)
        for (int a = 0; a < 3; ++a) { bmin[a]=std::min(bmin[a],centroid[f].comp(a)); bmax[a]=std::max(bmax[a],centroid[f].comp(a)); }
    float diag = 0; for (int a=0;a<3;++a) diag += (bmax[a]-bmin[a])*(bmax[a]-bmin[a]); diag = std::sqrt(diag);
    const float cell = std::max(1e-6f, diag * 0.02f);
    auto key = [&](float x,float y,float z){ return std::make_tuple(int(std::floor(x/cell)),int(std::floor(y/cell)),int(std::floor(z/cell))); };
    std::map<std::tuple<int,int,int>, std::vector<int>> grid;
    for (int f = 0; f < faceCount; ++f) {
        const Vec3& c = centroid[f];
        grid[key(c.comp(0),c.comp(1),c.comp(2))].push_back(f);
    }
    auto nearestFace = [&](float x,float y,float z)->int{
        int best=-1; float bd=1e30f; auto[kx,ky,kz]=key(x,y,z);
        for(int dx=-1;dx<=1;++dx)for(int dy=-1;dy<=1;++dy)for(int dz=-1;dz<=1;++dz){
            auto it=grid.find(std::make_tuple(kx+dx,ky+dy,kz+dz)); if(it==grid.end())continue;
            for(int f:it->second){const Vec3&c=centroid[f];
                float d=(c.comp(0)-x)*(c.comp(0)-x)+(c.comp(1)-y)*(c.comp(1)-y)+(c.comp(2)-z)*(c.comp(2)-z);
                if(d<bd){bd=d;best=f;}}}
        return best;
    };

    // For each near-seam candidate face, look at its mirror. If the mirror is a
    // limb but this face is torso (or vice versa), adopt the mirrored decision —
    // mapped to THIS side's limb label. Take the UNION of "is a limb" across the
    // mirror pair so both legs get the more-inclusive (symmetric) boundary; a
    // face becomes torso only if BOTH it and its mirror are torso.
    std::vector<int> out = faceLabels;
    int relabelled = 0;
    for (int f = 0; f < faceCount; ++f) {
        if (hop[f] < 0) continue;
        const int lf = faceLabels[f];
        const bool isLimb = (lf == limbLeft || lf == limbRight);
        if (!isLimb && lf != shared) continue;
        const Vec3& c = centroid[f];
        // reflect lateral about the sagittal centre
        float mx=c.comp(0), my=c.comp(1), mz=c.comp(2);
        const float ml = 2.0f*latCentre - c.comp(lat);
        if (lat==0) mx=ml; else if (lat==1) my=ml; else mz=ml;
        const int mf = nearestFace(mx,my,mz);
        if (mf < 0) continue;
        const int lm = faceLabels[mf];
        const bool mirrorIsLimb = (lm == limbLeft || lm == limbRight);
        // decide the symmetric label for THIS face by its lateral side
        const bool positive = (c.comp(lat) >= latCentre);
        const int sideLimb = limbOnSide(positive);
        int want = lf;
        if (isLimb && mirrorIsLimb) {
            want = sideLimb;              // both agree it's a limb → this side's limb
        } else if (isLimb && !mirrorIsLimb) {
            want = sideLimb;              // union: keep as limb (mirror will follow)
        } else if (!isLimb && mirrorIsLimb) {
            want = sideLimb;              // union: torso here but limb across → make limb
        } else {
            want = shared;               // both torso
        }
        if (want != lf) { out[f] = want; ++relabelled; }
    }
    faceLabels.swap(out);
    return relabelled;
}

int MeshSegmenter::cleanupLabelIslands(std::vector<int>& faceLabels,
                                       const uint32_t* indices, int indexCount,
                                       int minFaces, float maxFraction)
{
    const int faceCount = static_cast<int>(faceLabels.size());
    if (faceCount <= 0 || !indices || indexCount < faceCount * 3 || minFaces <= 0)
        return 0;

    const std::vector<std::vector<int>> adj = buildFaceAdjacency(indices, faceCount);
    int totalRelabelled = 0;

    // Iterate: reabsorbing one island can shrink a neighbour below threshold or
    // merge two islands. Bounded pass count guards against oscillation.
    for (int pass = 0; pass < 8; ++pass) {
        // 1) Per-label total face counts (for the fraction gate).
        std::unordered_map<int, int> labelTotal;
        for (int l : faceLabels)
            ++labelTotal[l];

        // 2) Connected face-islands (same-label edge-adjacency), building the
        //    id→faces membership ONCE so later steps never rescan all faces per
        //    island (keeps the pass O(F+E), not O(F·islands) — the fragmented
        //    case Codex flagged). islandLabel/Size parallel islandFaces.
        std::vector<int> islandOf(faceCount, -1);
        std::vector<int> islandLabel;
        std::vector<std::vector<int>> islandFaces;
        for (int f = 0; f < faceCount; ++f) {
            if (islandOf[f] != -1) continue;
            const int id = static_cast<int>(islandLabel.size());
            const int lbl = faceLabels[f];
            islandLabel.push_back(lbl);
            islandFaces.emplace_back();
            std::vector<int> stack{ f };
            islandOf[f] = id;
            while (!stack.empty()) {
                const int cur = stack.back(); stack.pop_back();
                islandFaces[id].push_back(cur);
                for (int nb : adj[cur]) {
                    if (islandOf[nb] == -1 && faceLabels[nb] == lbl) {
                        islandOf[nb] = id;
                        stack.push_back(nb);
                    }
                }
            }
        }
        const int islandCount = static_cast<int>(islandLabel.size());

        // 3) A label's LARGEST island is its main body and is ALWAYS protected,
        //    regardless of size — so a legitimately small part (flower, chimney,
        //    window, a low-poly limb) with only one island is never absorbed
        //    (Codex P1). Only NON-largest islands are reabsorption candidates.
        std::unordered_map<int, int> largestIslandForLabel; // label → island id
        for (int id = 0; id < islandCount; ++id) {
            const int lbl = islandLabel[id];
            auto it = largestIslandForLabel.find(lbl);
            if (it == largestIslandForLabel.end() ||
                islandFaces[id].size() > islandFaces[it->second].size())
                largestIslandForLabel[lbl] = id;
        }

        // 4) Reabsorb each stray (small, non-largest) island into the majority
        //    boundary-neighbour part.
        int relabelledThisPass = 0;
        for (int id = 0; id < islandCount; ++id) {
            const int lbl = islandLabel[id];
            if (largestIslandForLabel[lbl] == id)
                continue; // the part's main body — always kept
            const int sz = static_cast<int>(islandFaces[id].size());
            const int total = labelTotal[lbl];
            // A non-largest island is a stray if it's small by EITHER measure:
            // below the absolute face floor (a junction sliver) OR a tiny
            // fraction of its label (a fleck off a big part). OR — not AND — so
            // a 2-face sliver off a modest 50-face part still counts (2 is below
            // the 32 floor even though it's not below 2% of 50). A genuinely
            // LARGE secondary island (>= minFaces AND a real fraction) survives,
            // preserving legitimately multi-island parts (two ears, symmetric
            // pairs sharing a label).
            const bool strayBySize = sz < minFaces;
            const bool strayByFrac = total > 0 &&
                static_cast<float>(sz) < maxFraction * static_cast<float>(total);
            if (!(strayBySize || strayByFrac))
                continue; // a large secondary island (real multi-part) survives

            // Majority label among faces adjacent across the island boundary
            // (scans only THIS island's faces via the precomputed membership).
            std::unordered_map<int, int> votes;
            for (int f : islandFaces[id])
                for (int nb : adj[f])
                    if (islandOf[nb] != id)
                        ++votes[faceLabels[nb]];
            if (votes.empty())
                continue; // fully isolated island (no boundary) — leave it.
            int bestLabel = lbl, bestVotes = -1;
            for (const auto& kv : votes) {
                if (kv.second > bestVotes ||
                    (kv.second == bestVotes && kv.first < bestLabel)) {
                    bestVotes = kv.second;
                    bestLabel = kv.first;
                }
            }
            if (bestLabel == lbl)
                continue;
            for (int f : islandFaces[id]) {
                faceLabels[f] = bestLabel;
                ++relabelledThisPass;
            }
        }

        totalRelabelled += relabelledThisPass;
        if (relabelledThisPass == 0)
            break; // stable
    }
    return totalRelabelled;
}

void MeshSegmenter::vertexLabelsFromFaces(std::vector<int>& vertexLabels,
                                          const std::vector<int>& faceLabels,
                                          const uint32_t* indices, int indexCount)
{
    const int vc = static_cast<int>(vertexLabels.size());
    if (vc <= 0 || indexCount < 3)
        return;
    const int faceCount = static_cast<int>(faceLabels.size());
    // Per-vertex label votes from the faces referencing it.
    std::vector<std::unordered_map<int, int>> votes(vc);
    for (int f = 0; f < faceCount && f * 3 + 2 < indexCount; ++f) {
        const int lbl = faceLabels[f];
        for (int k = 0; k < 3; ++k) {
            const int v = static_cast<int>(indices[f * 3 + k]);
            if (v >= 0 && v < vc)
                ++votes[v][lbl];
        }
    }
    for (int v = 0; v < vc; ++v) {
        if (votes[v].empty()) continue; // unreferenced vertex keeps its label
        int best = vertexLabels[v], bestVotes = -1;
        for (const auto& kv : votes[v]) {
            if (kv.second > bestVotes || (kv.second == bestVotes && kv.first < best)) {
                bestVotes = kv.second;
                best = kv.first;
            }
        }
        vertexLabels[v] = best;
    }
}

// ---------------------------------------------------------------------------
// Geometric fallback
// ---------------------------------------------------------------------------

MeshSegmenter::Result MeshSegmenter::segmentGeometric(const float* positions, int vertexCount,
                                                      const uint32_t* indices, int indexCount,
                                                      const Options& opts,
                                                      const int* boneProximity)
{
    Result r;
    if (!positions || vertexCount <= 0) {
        r.error = QStringLiteral("MeshSegmenter: empty geometry.");
        return r;
    }
    const int up = std::clamp(opts.upAxis, 0, 2);
    const int side = (up == 0) ? 2 : 0;   // a lateral axis distinct from up

    // Whole-mesh AABB.
    std::array<float, 3> mn{ positions[0], positions[1], positions[2] };
    std::array<float, 3> mx = mn;
    for (int v = 0; v < vertexCount; ++v)
        for (int a = 0; a < 3; ++a) {
            mn[a] = std::min(mn[a], positions[3*v + a]);
            mx[a] = std::max(mx[a], positions[3*v + a]);
        }
    const float upSpan = std::max(1e-6f, mx[up] - mn[up]);
    const float sideMid = 0.5f * (mn[side] + mx[side]);

    // Category-aware fallback (#818 B2): Body keeps the original humanoid
    // heuristic; the other categories get a coarse up-band split — crude, but
    // deterministic and better than body labels on a tree/car/house.
    const Category cat = (opts.category == Category::Auto) ? Category::Body
                                                           : opts.category;
    const Part defaultPart = (cat == Category::Vegetation) ? Part::Trunk
                           : (cat == Category::Vehicle)    ? Part::VehicleBody
                           : (cat == Category::Building)   ? Part::Wall
                                                           : Part::Torso;
    r.vertexLabels.assign(vertexCount, static_cast<int>(defaultPart));

    // If we have rig bone-proximity hints, they're authoritative per-vertex
    // (bone-name → part is a BODY concept; other categories have no rig prior).
    const bool haveBone = (boneProximity != nullptr && cat == Category::Body);

    // Otherwise classify by connected-component island, using each island's
    // centroid in normalized up/side space. Per-island keeps a limb coherent
    // even where it overlaps the torso's bounding box.
    std::vector<int> island;
    const int nIslands = connectedComponents(vertexCount, indices, indexCount, island);

    auto classify = [&](float upN, float lateral) -> Part {
        // upN in [0,1] along up axis; lateral = signed offset from mid (>0 one side).
        switch (cat) {
        case Category::Vegetation:
            return upN > 0.5f ? Part::Foliage : Part::Trunk;
        case Category::Vehicle:
            return upN < 0.30f ? Part::Wheel : Part::VehicleBody;
        case Category::Building:
            return upN > 0.72f ? Part::Roof : Part::Wall;
        default: break;                                 // Body below
        }
        if (upN > 0.82f) return Part::Head;
        if (upN < 0.45f) {                              // lower body → legs
            return lateral >= 0.0f ? Part::RightLeg : Part::LeftLeg;
        }
        // mid band: torso in the centre, arms out to the sides.
        const float lateralSpan = std::max(1e-6f, mx[side] - mn[side]);
        if (std::abs(lateral) > 0.28f * lateralSpan)
            return lateral >= 0.0f ? Part::RightArm : Part::LeftArm;
        return Part::Torso;
    };

    // 1) ALWAYS compute the spatial (geometric) label first, for every vertex.
    if (nIslands > 1) {
        // Per-island centroid classification.
        std::vector<std::array<double, 3>> sum(nIslands, { 0, 0, 0 });
        std::vector<int> cnt(nIslands, 0);
        for (int v = 0; v < vertexCount; ++v) {
            const int is = island[v];
            for (int a = 0; a < 3; ++a) sum[is][a] += positions[3*v + a];
            ++cnt[is];
        }
        std::vector<int> islandLabel(nIslands, static_cast<int>(defaultPart));
        for (int is = 0; is < nIslands; ++is) {
            if (cnt[is] == 0) continue;
            const float cu = static_cast<float>(sum[is][up] / cnt[is]);
            const float cs = static_cast<float>(sum[is][side] / cnt[is]);
            islandLabel[is] = static_cast<int>(classify((cu - mn[up]) / upSpan, cs - sideMid));
        }
        for (int v = 0; v < vertexCount; ++v)
            r.vertexLabels[v] = islandLabel[island[v]];
    } else {
        // Single connected blob (typical for a humanoid body mesh): classify
        // each vertex directly by its own position.
        for (int v = 0; v < vertexCount; ++v) {
            const float upN = (positions[3*v + up] - mn[up]) / upSpan;
            const float lateral = positions[3*v + side] - sideMid;
            r.vertexLabels[v] = static_cast<int>(classify(upN, lateral));
        }
    }

    // 2) Override with VALID rig bone-proximity hints only. An unknown/invalid
    // hint (-1, or out of range) leaves the spatial label intact — a partially
    // hinted rig must not collapse its unhinted vertices to Torso.
    if (haveBone)
        applyBoneHints(r.vertexLabels, boneProximity);

    r.faceLabels = facesFromVertexLabels(r.vertexLabels, indices, indexCount);
    // Clean the part boundaries for a printable split (#863): straighten ragged
    // seams (leg/torso fringe) then reabsorb stray junction islands; reconcile
    // vertex labels only if the faces actually changed.
    if (opts.cleanupIslands) {
        int changed = 0;
        changed += smoothLabelBoundaries(r.faceLabels, indices, indexCount,
                                         opts.boundarySmoothIterations);
        // Level the mirror-limb cuts (legs/arms) to a shared horizontal line so
        // an explode is symmetric — BODY only, scoped to the limb↔torso seam.
        // LEGS only: they attach to the torso along a ~horizontal hip seam, so a
        // shared up-axis cut level makes both legs symmetric. Arms attach along a
        // VERTICAL (lateral) seam — a horizontal level there is meaningless and
        // collapses them into the torso — so arms are deliberately excluded.
        if (opts.levelLimbCuts && cat == Category::Body) {
            changed += levelLimbCut(r.faceLabels, positions, vertexCount,
                                    indices, indexCount,
                                    (int)Part::LeftLeg, (int)Part::RightLeg,
                                    (int)Part::Torso, opts.upAxis);
        }
        // Planar recut for a knife-straight seam (torso/leg fringe, level legs).
        if (opts.planarRecut)
            changed += planarBoundaryRecut(r.faceLabels, positions, vertexCount,
                                           indices, indexCount);
        // Island pass last: recut/smoothing can leave a tiny stray to reabsorb.
        changed += cleanupLabelIslands(r.faceLabels, indices, indexCount,
                                       opts.islandMinFaces, opts.islandMaxFraction);
        if (changed > 0)
            vertexLabelsFromFaces(r.vertexLabels, r.faceLabels, indices, indexCount);
    }
    r.ok = true;
    r.usedModel = false;
    r.category = cat;
    return r;
}

// ---------------------------------------------------------------------------
// Model management
// ---------------------------------------------------------------------------

namespace {
// Shared first-use download for the segment/ model family. Returns the local
// path, or empty when offline/disabled/failed.
QString ensureSegmentFileBlocking(const QString& dest, const QString& fileName,
                                  const QString& label)
{
#ifndef ENABLE_ONNX
    Q_UNUSED(dest); Q_UNUSED(fileName); Q_UNUSED(label);
    return {};
#else
    if (QFileInfo::exists(dest)) return dest;
    if (!qEnvironmentVariableIsEmpty("QTMESH_SEGMENT_NO_DOWNLOAD"))
        return {};

    QString base;
    {
        QSettings s;
        base = s.value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_SEGMENT_MODEL_BASE_URL");
            base = env.isEmpty() ? QString::fromLatin1(kDefaultModelBaseUrl)
                                 : QString::fromUtf8(env);
        }
    }
    if (base.isEmpty()) return {};
    if (!base.endsWith('/')) base += '/';

    auto* dl = ModelDownloader::instance();
    if (!dl) return {};

    QDir().mkpath(QFileInfo(dest).absolutePath());
    const QString url = base + fileName;

    QEventLoop loop;
    bool ok = false, timedOut = false;
    auto onDone = QObject::connect(dl, &ModelDownloader::downloadCompleted, &loop,
        [&](const QString& name, const QString&) { if (name == label) { ok = true; loop.quit(); } });
    auto onErr = QObject::connect(dl, &ModelDownloader::downloadError, &loop,
        [&](const QString& name, const QString&) { if (name == label) { ok = false; loop.quit(); } });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() { timedOut = true; loop.quit(); });
    timeout.start(300000);   // 5 min — every segment model is small (~MBs)

    dl->startDownload(url, dest, label);
    loop.exec();

    QObject::disconnect(onDone);
    QObject::disconnect(onErr);
    if (timedOut && dl) dl->cancelDownload();

    return (ok && !timedOut && QFileInfo::exists(dest)) ? dest : QString();
#endif
}
} // namespace

QString MeshSegmenter::ensureModelBlocking(Category c)
{
    return ensureSegmentFileBlocking(
        modelPath(c), modelFileName(c),
        QStringLiteral("Mesh segmentation model (%1)").arg(categoryName(c)));
}

QString MeshSegmenter::ensureClassifierModelBlocking()
{
    return ensureSegmentFileBlocking(
        classifierModelPath(), QString::fromLatin1(kClassifierModelFile),
        QStringLiteral("Mesh category classifier"));
}

// ---------------------------------------------------------------------------
// ONNX path
// ---------------------------------------------------------------------------

MeshSegmenter::Category MeshSegmenter::resolveCategoryBlocking(const float* positions,
                                                               int vertexCount,
                                                               const Options& opts)
{
    if (opts.category != Category::Auto) return opts.category;
#ifndef ENABLE_ONNX
    Q_UNUSED(positions); Q_UNUSED(vertexCount);
    return Category::Body;
#else
    if (!positions || vertexCount <= 0) return Category::Body;
    const QString cp = ensureClassifierModelBlocking();
    if (cp.isEmpty()) return Category::Body;
    return classifyCategory(positions, vertexCount, cp, opts);
#endif
}

#ifndef ENABLE_ONNX

MeshSegmenter::Category MeshSegmenter::classifyCategory(const float*, int,
                                                        const QString&, const Options&)
{
    return Category::Body;
}

MeshSegmenter::Result MeshSegmenter::predict(const float* positions, int vertexCount,
                                             const uint32_t* indices, int indexCount,
                                             const QString& /*modelPath*/, const Options& opts,
                                             const int* boneProximity, const ProgressFn&)
{
    Result r = segmentGeometric(positions, vertexCount, indices, indexCount, opts, boneProximity);
    if (r.ok)
        r.fallbackReason = QStringLiteral(
            "Mesh segmentation needs an ONNX-enabled build (rebuild with "
            "-DENABLE_ONNX) — used the geometric fallback.");
    return r;
}

#else // ENABLE_ONNX

namespace {
// The models are trained on +Y-up point clouds in a centred unit box. This
// frame captures the normalisation + the up-axis remap so the sampled cloud
// and the per-vertex scatter agree (`axisFor[outComponent] = sourceComponent`:
// up → Y(1), the two remaining source axes fill X(0)/Z(2) in ascending order).
struct CloudFrame {
    std::array<float,3> centre{};
    float inv = 1.0f;
    std::array<int,3> axisFor{ 0, 1, 2 };
};

CloudFrame computeCloudFrame(const float* positions, int vertexCount, int upAxis)
{
    CloudFrame f;
    std::array<float,3> mn{positions[0],positions[1],positions[2]}, mx = mn;
    for (int v = 0; v < vertexCount; ++v)
        for (int a = 0; a < 3; ++a) {
            mn[a] = std::min(mn[a], positions[3*v+a]);
            mx[a] = std::max(mx[a], positions[3*v+a]);
        }
    f.centre = { 0.5f*(mn[0]+mx[0]), 0.5f*(mn[1]+mx[1]), 0.5f*(mn[2]+mx[2]) };
    float half = 0.0f;
    for (int a = 0; a < 3; ++a) half = std::max(half, 0.5f*(mx[a]-mn[a]));
    f.inv = half > 1e-6f ? 1.0f/half : 1.0f;

    const int up = (upAxis >= 0 && upAxis <= 2) ? upAxis : 1;
    if (up != 1) {
        f.axisFor[1] = up;                 // model Y  ← mesh up axis
        int fill = 0;
        for (int a = 0; a < 3; ++a) {
            if (a == up) continue;
            f.axisFor[fill == 0 ? 0 : 2] = a;   // X then Z get the other two
            ++fill;
        }
    }
    return f;
}

// Deterministic point sample into the model frame. Small meshes include every
// vertex once so no vertex keeps the default label; large meshes are sampled
// across the whole vertex buffer instead of taking the first N vertices.
void sampleCloud(const float* positions, int vertexCount, const CloudFrame& f,
                 int N, std::vector<float>& pts, std::vector<int>* srcVert)
{
    std::mt19937 rng(0x5e6u);  // NOSONAR — non-crypto, fixed for reproducibility
    std::uniform_int_distribution<int> pick(0, vertexCount - 1);
    pts.assign(static_cast<size_t>(N) * 3, 0.0f);
    if (srcVert) srcVert->assign(N, 0);
    for (int i = 0; i < N; ++i) {
        int v = 0;
        if (vertexCount <= N) {
            v = (i < vertexCount) ? i : pick(rng);
        } else {
            const double t = (static_cast<double>(i) + 0.5) / static_cast<double>(N);
            v = std::clamp(static_cast<int>(t * static_cast<double>(vertexCount)),
                           0, vertexCount - 1);
        }
        if (srcVert) (*srcVert)[i] = v;
        for (int c = 0; c < 3; ++c) {
            const int a = f.axisFor[c];
            pts[3*i+c] = (positions[3*v+a]-f.centre[a])*f.inv;
        }
    }
}
} // namespace

MeshSegmenter::Result MeshSegmenter::predict(const float* positions, int vertexCount,
                                             const uint32_t* indices, int indexCount,
                                             const QString& modelPath, const Options& opts,
                                             const int* boneProximity, const ProgressFn& progress)
{
    // Auto is resolved by the CALLER (resolveCategoryBlocking) — a still-Auto
    // value here means "no classifier available", which degrades to Body.
    const Category cat = (opts.category == Category::Auto) ? Category::Body
                                                           : opts.category;
    auto fallback = [&](const QString& why) -> Result {
        Result r = segmentGeometric(positions, vertexCount, indices, indexCount, opts, boneProximity);
        if (r.ok) r.fallbackReason = why;
        return r;
    };
    if (opts.forceFallback)
        return fallback(QStringLiteral("Geometric fallback forced by request."));
    if (modelPath.isEmpty() || !QFileInfo::exists(modelPath))
        return fallback(QStringLiteral("Segmentation model not found — used the geometric fallback."));
    if (!positions || vertexCount <= 0)
        return fallback(QStringLiteral("Empty geometry — used the geometric fallback."));

    try {
        const CloudFrame frame = computeCloudFrame(positions, vertexCount, opts.upAxis);
        const int N = std::max(256, opts.samplePoints);
        std::vector<float> pts;
        std::vector<int> srcVert;
        sampleCloud(positions, vertexCount, frame, N, pts, &srcVert);

        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_segment");
        Ort::SessionOptions so;
        OnnxRuntimeSettings::configureSessionOptions(so);
        so.SetIntraOpNumThreads(1);
#ifdef _WIN32
        const std::wstring wpath = modelPath.toStdWString();
        Ort::Session session(env, wpath.c_str(), so);
#else
        const std::string p = modelPath.toStdString();
        Ort::Session session(env, p.c_str(), so);
#endif
        Ort::AllocatorWithDefaultOptions alloc;
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        if (progress && !progress(0, 1)) return fallback(QStringLiteral("cancelled"));

        const std::array<int64_t,3> shape{ 1, N, 3 };
        Ort::Value input = Ort::Value::CreateTensor<float>(
            mem, pts.data(), pts.size(), shape.data(), shape.size());

        Ort::AllocatedStringPtr inName = session.GetInputNameAllocated(0, alloc);
        const char* inNames[] = { inName.get() };
        const size_t outCount = session.GetOutputCount();
        std::vector<Ort::AllocatedStringPtr> outHolders;
        std::vector<const char*> outNames;
        for (size_t i = 0; i < outCount; ++i) {
            outHolders.push_back(session.GetOutputNameAllocated(i, alloc));
            outNames.push_back(outHolders.back().get());
        }

        std::vector<Ort::Value> outputs = session.Run(
            Ort::RunOptions{nullptr}, inNames, &input, 1, outNames.data(), outNames.size());
        if (outputs.empty() || !outputs[0].IsTensor())
            return fallback(QStringLiteral("Segmentation model gave no output — used the geometric fallback."));

        // Output is per-point logits [1, N, C] (or [1, C, N]); argmax over the
        // CATEGORY's channel count, then map the local channel to the global
        // Part via the category's channel map.
        auto ti = outputs[0].GetTensorTypeAndShapeInfo();
        const auto oshape = ti.GetShape();
        const size_t elems = ti.GetElementCount();
        const std::vector<int> chanMap = categoryChannelMap(cat);
        const int C = static_cast<int>(chanMap.size());
        if (elems < static_cast<size_t>(N))
            return fallback(QStringLiteral("Segmentation output too small — used the geometric fallback."));
        const int chan = (!oshape.empty() && oshape.back() > 0)
                             ? static_cast<int>(oshape.back()) : C;
        const bool channelsLast = (chan == C);
        const float* d = outputs[0].GetTensorData<float>();

        // Placeholder only — every sampled point and every scattered vertex is
        // overwritten below.
        const int defaultLabel = static_cast<int>(Part::Unknown);
        std::vector<int> pointLabel(N, defaultLabel);
        for (int i = 0; i < N; ++i) {
            int best = 0; float bestv = -1e30f;
            for (int c = 0; c < C; ++c) {
                const float val = channelsLast ? d[static_cast<size_t>(i)*chan + c]
                                               : d[static_cast<size_t>(c)*N + i];
                if (val > bestv) { bestv = val; best = c; }
            }
            pointLabel[i] = chanMap[best];
        }

        // Scatter sampled-point labels back to ALL vertices by nearest sampled
        // point (in normalised space). For samplePoints >= vertexCount the first
        // vertexCount samples cover every vertex 1:1, so this is exact.
        Result r;
        r.vertexLabels.assign(vertexCount, defaultLabel);
        if (vertexCount <= N) {
            for (int i = 0; i < vertexCount; ++i)
                r.vertexLabels[i] = pointLabel[i];
        } else {
            // brute-force nearest sampled point per vertex (N is small, ~4k).
            for (int v = 0; v < vertexCount; ++v) {
                // Same axis remap as the sampled points (`pts`), so distances are
                // compared in the SAME (model +Y-up) frame — not raw vertex order.
                const float vx=(positions[3*v+frame.axisFor[0]]-frame.centre[frame.axisFor[0]])*frame.inv;
                const float vy=(positions[3*v+frame.axisFor[1]]-frame.centre[frame.axisFor[1]])*frame.inv;
                const float vz=(positions[3*v+frame.axisFor[2]]-frame.centre[frame.axisFor[2]])*frame.inv;
                int best=0; float bd=1e30f;
                for (int i = 0; i < N; ++i) {
                    const float dx=vx-pts[3*i+0], dy=vy-pts[3*i+1], dz=vz-pts[3*i+2];
                    const float dd=dx*dx+dy*dy+dz*dz;
                    if (dd<bd){bd=dd;best=i;}
                }
                r.vertexLabels[v] = pointLabel[best];
            }
        }
        // Bone hints map bone names to BODY parts — only authoritative there.
        if (cat == Category::Body)
            applyBoneHints(r.vertexLabels, boneProximity);
        smoothLabelsByTopology(r.vertexLabels, indices, indexCount, 1);
        if (cat == Category::Body)
            applyBoneHints(r.vertexLabels, boneProximity);
        r.faceLabels = facesFromVertexLabels(r.vertexLabels, indices, indexCount);
        // Clean the part boundaries for a printable split (#863): straighten
        // ragged seams (leg/torso fringe) then reabsorb stray junction islands;
        // reconcile vertex labels only if the faces actually changed.
        if (opts.cleanupIslands) {
            int changed = 0;
            changed += smoothLabelBoundaries(r.faceLabels, indices, indexCount,
                                             opts.boundarySmoothIterations);
            // Level the mirror-limb cuts (legs/arms) to a shared horizontal line
            // so an explode is symmetric — BODY only, scoped to the limb↔torso
            // seam (fixes the model's lopsided, differently-heighted legs).
            // LEGS only (see the geometric path for why arms are excluded).
            if (opts.levelLimbCuts && cat == Category::Body) {
                changed += levelLimbCut(r.faceLabels, positions, vertexCount,
                                        indices, indexCount,
                                        (int)Part::LeftLeg, (int)Part::RightLeg,
                                        (int)Part::Torso, opts.upAxis);
            }
            // Planar recut for a knife-straight seam (torso/leg fringe, level legs).
            if (opts.planarRecut)
                changed += planarBoundaryRecut(r.faceLabels, positions, vertexCount,
                                               indices, indexCount);
            // Island pass last: recut/smoothing can leave a tiny stray.
            changed += cleanupLabelIslands(r.faceLabels, indices, indexCount,
                                           opts.islandMinFaces, opts.islandMaxFraction);
            if (changed > 0)
                vertexLabelsFromFaces(r.vertexLabels, r.faceLabels, indices, indexCount);
        }
        r.ok = true;
        r.usedModel = true;
        r.category = cat;
        return r;
    } catch (const Ort::Exception& e) {
        return fallback(QStringLiteral("Segmentation inference failed (%1) — used the geometric fallback.")
                            .arg(QString::fromUtf8(e.what())));
    } catch (const std::exception& e) {
        return fallback(QStringLiteral("Segmentation error (%1) — used the geometric fallback.")
                            .arg(QString::fromUtf8(e.what())));
    }
}

MeshSegmenter::Category MeshSegmenter::classifyCategory(const float* positions,
                                                        int vertexCount,
                                                        const QString& classifierPath,
                                                        const Options& opts)
{
    if (!positions || vertexCount <= 0) return Category::Body;
    if (classifierPath.isEmpty() || !QFileInfo::exists(classifierPath))
        return Category::Body;

    try {
        const CloudFrame frame = computeCloudFrame(positions, vertexCount, opts.upAxis);
        const int N = std::max(256, opts.samplePoints);
        std::vector<float> pts;
        sampleCloud(positions, vertexCount, frame, N, pts, nullptr);

        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_segment_cls");
        Ort::SessionOptions so;
        OnnxRuntimeSettings::configureSessionOptions(so);
        so.SetIntraOpNumThreads(1);
#ifdef _WIN32
        const std::wstring wpath = classifierPath.toStdWString();
        Ort::Session session(env, wpath.c_str(), so);
#else
        const std::string p = classifierPath.toStdString();
        Ort::Session session(env, p.c_str(), so);
#endif
        Ort::AllocatorWithDefaultOptions alloc;
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        const std::array<int64_t,3> shape{ 1, N, 3 };
        Ort::Value input = Ort::Value::CreateTensor<float>(
            mem, pts.data(), pts.size(), shape.data(), shape.size());

        Ort::AllocatedStringPtr inName = session.GetInputNameAllocated(0, alloc);
        const char* inNames[] = { inName.get() };
        Ort::AllocatedStringPtr outName = session.GetOutputNameAllocated(0, alloc);
        const char* outNames[] = { outName.get() };

        std::vector<Ort::Value> outputs = session.Run(
            Ort::RunOptions{nullptr}, inNames, &input, 1, outNames, 1);
        if (outputs.empty() || !outputs[0].IsTensor()) return Category::Body;

        // Output: [1, 4] logits over {body, vegetation, vehicle, building} —
        // the training-script CLASSIFIER_CLASSES order, which maps 1:1 onto
        // Category::Body..Building.
        auto ti = outputs[0].GetTensorTypeAndShapeInfo();
        const size_t elems = ti.GetElementCount();
        constexpr size_t kClasses =
            static_cast<size_t>(Category::CategoryCount) - 1;   // minus Auto
        if (elems < kClasses) return Category::Body;
        const float* d = outputs[0].GetTensorData<float>();
        size_t best = 0;
        for (size_t c = 1; c < kClasses; ++c)
            if (d[c] > d[best]) best = c;
        return static_cast<Category>(best + 1);   // 0 → Body, 1 → Vegetation, …
    } catch (const Ort::Exception&) {
        return Category::Body;
    } catch (const std::exception&) {
        return Category::Body;
    }
}

#endif // ENABLE_ONNX
