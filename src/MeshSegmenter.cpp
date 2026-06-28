#include "MeshSegmenter.h"

#include "ModelDownloader.h"

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
#include <numeric>
#include <unordered_map>

#ifdef ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#include <random>
#include <unordered_set>
#endif

namespace {
constexpr const char* kModelFile = "meshseg.onnx";
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/segment/";
constexpr const char* kBaseUrlSettingsKey = "ai/segmentModelBaseUrl";

const char* const kPartIds[] = {
    "unknown", "head", "torso", "left_arm", "right_arm", "left_leg", "right_leg",
};

// Normalise a bone name for matching: lowercase, strip a leading
// "mixamorig[N]:" / "bip01 " / "def_"-style prefix, drop separators so
// "LeftArm" / "L_Arm" / "arm.l" / "DEF-upper_arm.L" compare alike.
QString normaliseBoneName(const QString& raw)
{
    QString s = raw.toLower();
    const int colon = s.lastIndexOf(':');           // mixamorig:LeftArm → leftarm
    if (colon >= 0) s = s.mid(colon + 1);
    for (const char* p : {"def-", "def_", "def", "org-", "mch-", "ctrl-"})
        if (s.startsWith(QLatin1String(p))) { s = s.mid(int(qstrlen(p))); break; }
    s.remove(' ').remove('_').remove('-').remove('.');
    return s;
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
} // namespace

MeshSegmenter::Options::Options() = default;

int MeshSegmenter::partCount() { return static_cast<int>(Part::Count); }

QString MeshSegmenter::partName(int p)
{
    if (p < 0 || p >= static_cast<int>(Part::Count)) return QStringLiteral("unknown");
    return QString::fromLatin1(kPartIds[p]);
}
QString MeshSegmenter::partName(Part p) { return partName(static_cast<int>(p)); }

MeshSegmenter::Part MeshSegmenter::partForBoneName(const QString& boneName)
{
    const QString n = normaliseBoneName(boneName);
    if (n.isEmpty()) return Part::Unknown;
    const char side = boneSide(n);
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
             "tail", "spine1", "spine2", "ribcage", "belly", "root"}) && side == 0)
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

QString MeshSegmenter::modelPath()
{
    const QString dataPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataPath).filePath(QStringLiteral("ai_models/segment/")
                                   + QString::fromLatin1(kModelFile));
}

bool MeshSegmenter::modelPresent() { return QFileInfo::exists(modelPath()); }

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

    r.vertexLabels.assign(vertexCount, static_cast<int>(Part::Torso));

    // If we have rig bone-proximity hints, they're authoritative per-vertex.
    const bool haveBone = (boneProximity != nullptr);

    // Otherwise classify by connected-component island, using each island's
    // centroid in normalized up/side space. Per-island keeps a limb coherent
    // even where it overlaps the torso's bounding box.
    std::vector<int> island;
    const int nIslands = connectedComponents(vertexCount, indices, indexCount, island);

    auto classify = [&](float upN, float lateral) -> Part {
        // upN in [0,1] along up axis; lateral = signed offset from mid (>0 one side).
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
        std::vector<int> islandLabel(nIslands, static_cast<int>(Part::Torso));
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
    if (haveBone) {
        for (int v = 0; v < vertexCount; ++v) {
            const int bp = boneProximity[v];
            if (bp >= 0 && bp < static_cast<int>(Part::Count))
                r.vertexLabels[v] = bp;
        }
    }

    r.faceLabels = facesFromVertexLabels(r.vertexLabels, indices, indexCount);
    r.ok = true;
    r.usedModel = false;
    return r;
}

// ---------------------------------------------------------------------------
// Model management
// ---------------------------------------------------------------------------

QString MeshSegmenter::ensureModelBlocking()
{
#ifndef ENABLE_ONNX
    return {};
#else
    const QString dest = modelPath();
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
    const QString url = base + QString::fromLatin1(kModelFile);
    const QString label = QStringLiteral("Mesh segmentation model");

    QEventLoop loop;
    bool ok = false, timedOut = false;
    auto onDone = QObject::connect(dl, &ModelDownloader::downloadCompleted, &loop,
        [&](const QString& name, const QString&) { if (name == label) { ok = true; loop.quit(); } });
    auto onErr = QObject::connect(dl, &ModelDownloader::downloadError, &loop,
        [&](const QString& name, const QString&) { if (name == label) { ok = false; loop.quit(); } });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() { timedOut = true; loop.quit(); });
    timeout.start(300000);   // 5 min — the model is small (~MBs)

    dl->startDownload(url, dest, label);
    loop.exec();

    QObject::disconnect(onDone);
    QObject::disconnect(onErr);
    if (timedOut && dl) dl->cancelDownload();

    return (ok && !timedOut && QFileInfo::exists(dest)) ? dest : QString();
#endif
}

// ---------------------------------------------------------------------------
// ONNX path
// ---------------------------------------------------------------------------

#ifndef ENABLE_ONNX

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

MeshSegmenter::Result MeshSegmenter::predict(const float* positions, int vertexCount,
                                             const uint32_t* indices, int indexCount,
                                             const QString& modelPath, const Options& opts,
                                             const int* boneProximity, const ProgressFn& progress)
{
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
        // Normalise vertices into a centred unit box (PointNet++ convention).
        std::array<float,3> mn{positions[0],positions[1],positions[2]}, mx = mn;
        for (int v = 0; v < vertexCount; ++v)
            for (int a = 0; a < 3; ++a) {
                mn[a] = std::min(mn[a], positions[3*v+a]);
                mx[a] = std::max(mx[a], positions[3*v+a]);
            }
        std::array<float,3> centre{ 0.5f*(mn[0]+mx[0]), 0.5f*(mn[1]+mx[1]), 0.5f*(mn[2]+mx[2]) };
        float half = 0.0f;
        for (int a = 0; a < 3; ++a) half = std::max(half, 0.5f*(mx[a]-mn[a]));
        const float inv = half > 1e-6f ? 1.0f/half : 1.0f;

        // The model is trained on +Y-up point clouds. If the mesh's up axis is
        // X or Z, remap coordinates so the up axis lands on Y before inference —
        // otherwise an x/z-up mesh is segmented in the wrong frame and head/legs
        // mislabel. `axisFor[outComponent] = sourceComponent`: up → Y(1), and the
        // two remaining source axes fill X(0)/Z(2) in ascending order.
        const int up = (opts.upAxis >= 0 && opts.upAxis <= 2) ? opts.upAxis : 1;
        std::array<int,3> axisFor{ 0, 1, 2 };
        if (up != 1) {
            axisFor[1] = up;                 // model Y  ← mesh up axis
            int fill = 0;
            for (int a = 0; a < 3; ++a) {
                if (a == up) continue;
                axisFor[fill == 0 ? 0 : 2] = a;   // X then Z get the other two
                ++fill;
            }
        }

        const int N = std::max(256, opts.samplePoints);
        // Deterministic point sample (with replacement if the mesh is small).
        std::mt19937 rng(0x5e6u);  // NOSONAR — non-crypto, fixed for reproducibility
        std::uniform_int_distribution<int> pick(0, vertexCount - 1);
        std::vector<float> pts(static_cast<size_t>(N) * 3);
        std::vector<int> srcVert(N);
        for (int i = 0; i < N; ++i) {
            const int v = (vertexCount >= N) ? (i < vertexCount ? i : pick(rng)) : pick(rng);
            srcVert[i] = v;
            for (int c = 0; c < 3; ++c) {
                const int a = axisFor[c];
                pts[3*i+c] = (positions[3*v+a]-centre[a])*inv;
            }
        }

        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_segment");
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(1);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef __APPLE__
        try { std::unordered_map<std::string,std::string> c; so.AppendExecutionProvider("CoreML", c); }
        catch (const Ort::Exception&) {}
#endif
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

        // Output is per-point logits [1, N, C] (or [1, C, N]); argmax → label.
        auto ti = outputs[0].GetTensorTypeAndShapeInfo();
        const auto oshape = ti.GetShape();
        const size_t elems = ti.GetElementCount();
        const int C = static_cast<int>(Part::Count);
        if (elems < static_cast<size_t>(N))
            return fallback(QStringLiteral("Segmentation output too small — used the geometric fallback."));
        const int chan = (!oshape.empty() && oshape.back() > 0)
                             ? static_cast<int>(oshape.back()) : C;
        const bool channelsLast = (chan == C);
        const float* d = outputs[0].GetTensorData<float>();

        std::vector<int> pointLabel(N, static_cast<int>(Part::Torso));
        for (int i = 0; i < N; ++i) {
            int best = 0; float bestv = -1e30f;
            for (int c = 0; c < C; ++c) {
                const float val = channelsLast ? d[static_cast<size_t>(i)*chan + c]
                                               : d[static_cast<size_t>(c)*N + i];
                if (val > bestv) { bestv = val; best = c; }
            }
            pointLabel[i] = best;
        }

        // Scatter sampled-point labels back to ALL vertices by nearest sampled
        // point (in normalised space). For samplePoints >= vertexCount the first
        // N points already cover every vertex 1:1, so this is exact.
        Result r;
        r.vertexLabels.assign(vertexCount, static_cast<int>(Part::Torso));
        if (vertexCount <= N) {
            for (int i = 0; i < N && i < vertexCount; ++i)
                r.vertexLabels[srcVert[i]] = pointLabel[i];
            // any vertex not directly sampled (shouldn't happen for vc<=N first-N) → nearest
        } else {
            // brute-force nearest sampled point per vertex (N is small, ~4k).
            for (int v = 0; v < vertexCount; ++v) {
                const float vx=(positions[3*v+0]-centre[0])*inv;
                const float vy=(positions[3*v+1]-centre[1])*inv;
                const float vz=(positions[3*v+2]-centre[2])*inv;
                int best=0; float bd=1e30f;
                for (int i = 0; i < N; ++i) {
                    const float dx=vx-pts[3*i+0], dy=vy-pts[3*i+1], dz=vz-pts[3*i+2];
                    const float dd=dx*dx+dy*dy+dz*dz;
                    if (dd<bd){bd=dd;best=i;}
                }
                r.vertexLabels[v] = pointLabel[best];
            }
        }
        r.faceLabels = facesFromVertexLabels(r.vertexLabels, indices, indexCount);
        r.ok = true;
        r.usedModel = true;
        return r;
    } catch (const Ort::Exception& e) {
        return fallback(QStringLiteral("Segmentation inference failed (%1) — used the geometric fallback.")
                            .arg(QString::fromUtf8(e.what())));
    } catch (const std::exception& e) {
        return fallback(QStringLiteral("Segmentation error (%1) — used the geometric fallback.")
                            .arg(QString::fromUtf8(e.what())));
    }
}

#endif // ENABLE_ONNX
