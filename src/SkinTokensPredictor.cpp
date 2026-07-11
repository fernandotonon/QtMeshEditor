#include "SkinTokensPredictor.h"
#include "ModelDownloader.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <set>
#include <thread>

#ifdef ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace {

constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/skintokens/";
constexpr const char* kBaseUrlSettingsKey = "ai/skintokensModelBaseUrl";

// The five graphs + the manifest the export script produces.
const char* kFiles[] = {
    "skintokens.json",
    "mesh_cond.onnx",
    "vae_cond.onnx",
    "embed.onnx",
    "decoder.onnx",
    "decoder.onnx.data",   // Qwen3 weights as external data — ORT 1.20.1
                           // SIGSEGVs parsing the 1.66 GB single-file proto
    "skin_decode.onnx",
};

SkinTokensPredictor::Result failResult(const QString& why)
{
    SkinTokensPredictor::Result r;
    r.ok = false;
    r.error = why;
    return r;
}

// Stage tracing for crash diagnosis: QTMESH_SKINTOKENS_DEBUG=1.
void dbg(const char* msg)
{
    if (qEnvironmentVariableIsEmpty("QTMESH_SKINTOKENS_DEBUG")) return;
    fprintf(stderr, "[skintokens] %s\n", msg);
    fflush(stderr);
}

// Deterministic LCG so sampling never touches global RNG state (and
// reruns are reproducible).
struct Lcg {
    std::uint64_t s = 0x853c49e6748fea9bULL;
    double next() {   // [0,1)
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return double((s >> 11) & ((1ULL << 53) - 1)) / double(1ULL << 53);
    }
};

} // namespace

SkinTokensPredictor::Options::Options() = default;

// ─── Pure-data helpers ──────────────────────────────────────────────────────

int SkinTokensPredictor::discretize(double t, double lo, double hi,
                                    int numDiscrete)
{
    // Mirrors the upstream tokenizer: bin = (t - lo) / (hi - lo) * N,
    // clamped to [0, N-1].
    if (hi <= lo || numDiscrete <= 1) return 0;
    const double x = (t - lo) / (hi - lo) * double(numDiscrete);
    const int b = int(std::floor(x));
    return std::clamp(b, 0, numDiscrete - 1);
}

std::vector<std::int64_t> SkinTokensPredictor::tokenizeSkeleton(
    const std::vector<Joint>& joints, const TokenizerLayout& layout)
{
    std::vector<std::int64_t> out;
    const int J = int(joints.size());
    if (J == 0) return out;

    // Validate the DFS ordering: parents strictly before children,
    // single root at index 0.
    if (joints[0].parent != -1) return out;
    for (int i = 1; i < J; ++i) {
        if (joints[i].parent < 0 || joints[i].parent >= i) return out;
    }

    auto bin = [&](double v) {
        return std::int64_t(discretize(v, layout.rangeLo, layout.rangeHi,
                                       layout.numDiscrete));
    };

    out.push_back(layout.tokBos);
    if (layout.tokCls >= 0)
        out.push_back(layout.tokCls);
    else if (layout.tokClsNone >= 0)
        out.push_back(layout.tokClsNone);

    // branch[i] = parents[i] != i-1 (the upstream TokenizeInput rule;
    // the root emits a plain 3-coord record).
    for (int i = 0; i < J; ++i) {
        const bool branch = (i > 0) && (joints[i].parent != i - 1);
        if (branch) {
            const Joint& p = joints[joints[i].parent];
            out.push_back(layout.tokBranch);
            out.push_back(bin(p.pos[0]));
            out.push_back(bin(p.pos[1]));
            out.push_back(bin(p.pos[2]));
        }
        out.push_back(bin(joints[i].pos[0]));
        out.push_back(bin(joints[i].pos[1]));
        out.push_back(bin(joints[i].pos[2]));
    }
    out.push_back(layout.tokEos);
    return out;
}

void SkinTokensPredictor::transferWeights(
    const float* vertices, int vertexCount,
    const std::vector<std::array<float, 3>>& samplePositions,
    const std::vector<float>& sampleWeights, int numJoints,
    int maxInfluences,
    std::vector<Result::VertexWeights>& out)
{
    out.assign(std::size_t(std::max(0, vertexCount)), {});
    const int S = int(samplePositions.size());
    if (vertexCount <= 0 || S == 0 || numJoints <= 0) return;
    if (sampleWeights.size() < std::size_t(S) * std::size_t(numJoints)) return;
    const int maxK = std::clamp(maxInfluences, 1, 8);
    const int kNN  = std::min(8, S);

    // Coarse spatial grid over the samples for the k-NN queries.
    float mn[3] = { samplePositions[0][0], samplePositions[0][1],
                    samplePositions[0][2] };
    float mx[3] = { mn[0], mn[1], mn[2] };
    for (const auto& p : samplePositions)
        for (int a = 0; a < 3; ++a) {
            mn[a] = std::min(mn[a], p[a]);
            mx[a] = std::max(mx[a], p[a]);
        }
    const float ext = std::max({ mx[0] - mn[0], mx[1] - mn[1],
                                 mx[2] - mn[2], 1e-6f });
    const int   res = 32;
    const float cell = ext / res;
    auto cellOf = [&](float x, float y, float z, int c[3]) {
        c[0] = std::clamp(int((x - mn[0]) / cell), 0, res - 1);
        c[1] = std::clamp(int((y - mn[1]) / cell), 0, res - 1);
        c[2] = std::clamp(int((z - mn[2]) / cell), 0, res - 1);
    };
    std::vector<std::vector<int>> grid(std::size_t(res) * res * res);
    for (int s = 0; s < S; ++s) {
        int c[3];
        cellOf(samplePositions[s][0], samplePositions[s][1],
               samplePositions[s][2], c);
        grid[(std::size_t(c[2]) * res + c[1]) * res + c[0]].push_back(s);
    }

    std::vector<std::pair<float, int>> knn;
    std::vector<double> acc(static_cast<std::size_t>(numJoints));
    for (int v = 0; v < vertexCount; ++v) {
        const float px = vertices[3 * v + 0];
        const float py = vertices[3 * v + 1];
        const float pz = vertices[3 * v + 2];

        // Expanding ring search until we have >= kNN candidates.
        knn.clear();
        int c[3];
        cellOf(px, py, pz, c);
        for (int r = 0; r < res && int(knn.size()) < kNN; ++r) {
            for (int dz = -r; dz <= r; ++dz)
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx) {
                        if (std::max({ std::abs(dx), std::abs(dy),
                                       std::abs(dz) }) != r)
                            continue;
                        const int cx = c[0] + dx, cy = c[1] + dy,
                                  cz = c[2] + dz;
                        if (cx < 0 || cy < 0 || cz < 0 || cx >= res
                            || cy >= res || cz >= res)
                            continue;
                        for (const int s :
                             grid[(std::size_t(cz) * res + cy) * res + cx]) {
                            const float dxs = samplePositions[s][0] - px;
                            const float dys = samplePositions[s][1] - py;
                            const float dzs = samplePositions[s][2] - pz;
                            knn.emplace_back(
                                dxs * dxs + dys * dys + dzs * dzs, s);
                        }
                    }
        }
        std::sort(knn.begin(), knn.end());
        const int k = std::min<int>(kNN, int(knn.size()));
        if (k == 0) continue;

        // Inverse-distance blend of the k nearest samples' weight rows
        // (the upstream Asset.from_data recipe).
        std::fill(acc.begin(), acc.end(), 0.0);
        double wsum = 0.0;
        for (int i = 0; i < k; ++i) {
            const double w = 1.0 / (std::sqrt(double(knn[i].first)) + 1e-8);
            const float* row =
                sampleWeights.data() + std::size_t(knn[i].second) * numJoints;
            for (int j = 0; j < numJoints; ++j) acc[j] += w * row[j];
            wsum += w;
        }
        if (wsum <= 0.0) continue;

        // Keep the top maxK joints, clamp negatives, normalise.
        Result::VertexWeights& vw = out[v];
        for (int j = 0; j < numJoints; ++j) {
            const double w = std::max(0.0, acc[j] / wsum);
            if (w <= 0.0) continue;
            if (vw.count < maxK) {
                vw.jointIndices[vw.count] = j;
                vw.weights[vw.count] = w;
                ++vw.count;
            } else {
                int minIdx = 0;
                for (int i = 1; i < maxK; ++i)
                    if (vw.weights[i] < vw.weights[minIdx]) minIdx = i;
                if (w > vw.weights[minIdx]) {
                    vw.jointIndices[minIdx] = j;
                    vw.weights[minIdx] = w;
                }
            }
        }
        double sum = 0.0;
        for (int i = 0; i < vw.count; ++i) sum += vw.weights[i];
        if (sum > 0.0)
            for (int i = 0; i < vw.count; ++i) vw.weights[i] /= sum;
        else
            vw.count = 0;
    }
}

// ─── Paths + download plumbing ──────────────────────────────────────────────

bool SkinTokensPredictor::isAvailable()
{
#ifdef ENABLE_ONNX
    return true;
#else
    return false;
#endif
}

QString SkinTokensPredictor::modelDir()
{
    return QStandardPaths::writableLocation(
               QStandardPaths::AppDataLocation)
        + QStringLiteral("/ai_models/skintokens");
}

QString SkinTokensPredictor::manifestPath()
{
    return modelDir() + QStringLiteral("/skintokens.json");
}

bool SkinTokensPredictor::modelsPresent()
{
    for (const char* f : kFiles)
        if (!QFileInfo::exists(modelDir() + QLatin1Char('/')
                               + QLatin1String(f)))
            return false;
    return true;
}

QString SkinTokensPredictor::ensureModelBlocking()
{
#ifndef ENABLE_ONNX
    return {};
#else
    if (modelsPresent()) return manifestPath();
    if (!qEnvironmentVariableIsEmpty("QTMESH_SKINTOKENS_NO_DOWNLOAD"))
        return {};

    QString base;
    {
        QSettings s;
        base = s.value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_SKINTOKENS_MODEL_BASE_URL");
            base = env.isEmpty() ? QString::fromLatin1(kDefaultModelBaseUrl)
                                 : QString::fromUtf8(env);
        }
    }
    if (base.isEmpty()) return {};
    if (!base.endsWith('/')) base += '/';

    auto* dl = ModelDownloader::instance();
    if (!dl) return {};

    auto downloadOne = [&](const QString& fileName) -> bool {
        const QString dest = modelDir() + QLatin1Char('/') + fileName;
        if (QFileInfo::exists(dest)) return true;
        QDir().mkpath(QFileInfo(dest).absolutePath());
        const QString label =
            QStringLiteral("skintokens/%1").arg(fileName);
        QEventLoop loop;
        bool ok = false, timedOut = false;
        auto onDone = QObject::connect(
            dl, &ModelDownloader::downloadCompleted, &loop,
            [&](const QString& name, const QString&) {
                if (name == label) { ok = true; loop.quit(); }
            });
        auto onErr = QObject::connect(
            dl, &ModelDownloader::downloadError, &loop,
            [&](const QString& name, const QString&) {
                if (name == label) { ok = false; loop.quit(); }
            });
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop,
                         [&]() { timedOut = true; loop.quit(); });
        // The Qwen3-0.6B decoder graph is the big one (~2.4 GB fp32);
        // generous cap for slow links.
        timeout.start(3600000);
        dl->startDownload(base + fileName, dest, label);
        loop.exec();
        QObject::disconnect(onDone);
        QObject::disconnect(onErr);
        if (timedOut) dl->cancelDownload();
        return ok && !timedOut && QFileInfo::exists(dest);
    };

    for (const char* f : kFiles)
        if (!downloadOne(QLatin1String(f))) return {};
    return modelsPresent() ? manifestPath() : QString();
#endif
}

// ─── predict ────────────────────────────────────────────────────────────────

#ifndef ENABLE_ONNX

SkinTokensPredictor::Result SkinTokensPredictor::predict(
    const float*, int, const std::uint32_t*, std::size_t,
    const std::vector<Joint>&, const Options&, const ProgressFn&)
{
    return failResult(QStringLiteral(
        "SkinTokens requires an ONNX build — rebuild with -DENABLE_ONNX=ON."));
}

#else

namespace {

// Area-weighted surface sampling with per-sample (flat) normals.
void sampleSurface(const float* positions, int vertexCount,
                   const std::uint32_t* indices, std::size_t indexCount,
                   int numSamples,
                   std::vector<std::array<float, 3>>& pts,
                   std::vector<std::array<float, 3>>& nrm)
{
    pts.clear();
    nrm.clear();
    const std::size_t triCount = indexCount / 3;
    std::vector<double> cumArea;
    cumArea.reserve(triCount);
    double total = 0.0;
    auto vtx = [&](std::uint32_t i) {
        return std::array<double, 3>{ positions[3 * i], positions[3 * i + 1],
                                      positions[3 * i + 2] };
    };
    std::vector<std::array<double, 3>> triNormal(triCount, { 0, 0, 1 });
    for (std::size_t t = 0; t < triCount; ++t) {
        const auto a = vtx(indices[3 * t]);
        const auto b = vtx(indices[3 * t + 1]);
        const auto c = vtx(indices[3 * t + 2]);
        const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
        const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        double nx = uy * vz - uz * vy;
        double ny = uz * vx - ux * vz;
        double nz = ux * vy - uy * vx;
        const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-20) {
            triNormal[t] = { nx / len, ny / len, nz / len };
        }
        total += 0.5 * len;
        cumArea.push_back(total);
    }
    if (total <= 0.0 || triCount == 0) {
        // Degenerate surface: fall back to vertex positions.
        const int n = std::min(numSamples, vertexCount);
        for (int i = 0; i < n; ++i) {
            pts.push_back({ positions[3 * i], positions[3 * i + 1],
                            positions[3 * i + 2] });
            nrm.push_back({ 0.f, 1.f, 0.f });
        }
        return;
    }

    Lcg rng;
    pts.reserve(numSamples);
    nrm.reserve(numSamples);
    for (int s = 0; s < numSamples; ++s) {
        const double r = rng.next() * total;
        const auto it = std::lower_bound(cumArea.begin(), cumArea.end(), r);
        const std::size_t t = std::size_t(it - cumArea.begin());
        const auto a = vtx(indices[3 * t]);
        const auto b = vtx(indices[3 * t + 1]);
        const auto c = vtx(indices[3 * t + 2]);
        double u = rng.next(), v = rng.next();
        if (u + v > 1.0) { u = 1.0 - u; v = 1.0 - v; }
        pts.push_back({ float(a[0] + u * (b[0] - a[0]) + v * (c[0] - a[0])),
                        float(a[1] + u * (b[1] - a[1]) + v * (c[1] - a[1])),
                        float(a[2] + u * (b[2] - a[2]) + v * (c[2] - a[2])) });
        nrm.push_back({ float(triNormal[t][0]), float(triNormal[t][1]),
                        float(triNormal[t][2]) });
    }
}

} // namespace

SkinTokensPredictor::Result SkinTokensPredictor::predict(
    const float* positions, int vertexCount,
    const std::uint32_t* indices, std::size_t indexCount,
    const std::vector<Joint>& joints,
    const Options& opts,
    const ProgressFn& progress)
{
    if (!positions || vertexCount < 3 || !indices || indexCount < 3)
        return failResult(QStringLiteral("SkinTokens: degenerate mesh input."));
    if (joints.empty())
        return failResult(QStringLiteral("SkinTokens: no joints."));
    if (!modelsPresent())
        return failResult(QStringLiteral(
            "SkinTokens: models not present (download them first)."));

    dbg("reading manifest");
    // ── Manifest ────────────────────────────────────────────────────
    QFile mf(manifestPath());
    if (!mf.open(QIODevice::ReadOnly))
        return failResult(QStringLiteral("SkinTokens: cannot read manifest."));
    const QJsonObject man = QJsonDocument::fromJson(mf.readAll()).object();
    if (man.value("schema").toString() != QLatin1String("qtmesh-skintokens-onnx-v1"))
        return failResult(QStringLiteral("SkinTokens: unknown manifest schema."));

    const int N              = man.value("num_points").toInt(8192);
    const int tokensPerSkin  = man.value("tokens_per_skin").toInt(0);
    const QJsonObject tokCfg = man.value("tokenizer").toObject();
    const QJsonObject llmCfg = man.value("llm").toObject();
    TokenizerLayout layout;
    layout.numDiscrete = tokCfg.value("num_discrete").toInt(256);
    {
        const auto cr = tokCfg.value("continuous_range").toArray();
        if (cr.size() == 2) {
            layout.rangeLo = cr.at(0).toDouble(-1.0);
            layout.rangeHi = cr.at(1).toDouble(1.0);
        }
    }
    layout.tokBranch  = tokCfg.value("token_id_branch").toInt(256);
    layout.tokBos     = tokCfg.value("token_id_bos").toInt(257);
    layout.tokEos     = tokCfg.value("token_id_eos").toInt(258);
    layout.tokPad     = tokCfg.value("token_id_pad").toInt(259);
    layout.tokClsNone = tokCfg.value("token_id_cls_none").toInt(-1);
    layout.tokCls     = tokCfg.value("cls_token_id").toObject()
                            .value("articulation").toInt(-1);
    const int tokVocab   = tokCfg.value("vocab_size").toInt(0);
    const int fsqVocab   = man.value("fsq_codebook_size").toInt(0);
    const int fullVocab  = llmCfg.value("full_vocab_size").toInt(0);
    if (tokensPerSkin <= 0 || tokVocab <= 0 || fsqVocab <= 0 || fullVocab <= 0)
        return failResult(QStringLiteral("SkinTokens: manifest missing config."));

    const int J = int(joints.size());

    // ── Normalise mesh + joints into the model frame ────────────────
    // Fit the mesh AABB into continuous_range (uniform scale, centred),
    // and put the JOINTS through the same transform — coordinate
    // consistency between the point cloud and the tokenized skeleton
    // is what the model was trained on.
    // Upstream AugmentAffine parity: the AABB includes the JOINTS,
    // uniform scale, exact fit into continuous_range (no margin).
    double mn[3] = { std::numeric_limits<double>::max(),
                     std::numeric_limits<double>::max(),
                     std::numeric_limits<double>::max() };
    double mx[3] = { -mn[0], -mn[1], -mn[2] };
    for (int v = 0; v < vertexCount; ++v)
        for (int a = 0; a < 3; ++a) {
            mn[a] = std::min<double>(mn[a], positions[3 * v + a]);
            mx[a] = std::max<double>(mx[a], positions[3 * v + a]);
        }
    for (const Joint& j : joints)
        for (int a = 0; a < 3; ++a) {
            mn[a] = std::min(mn[a], j.pos[a]);
            mx[a] = std::max(mx[a], j.pos[a]);
        }
    const double extent = std::max({ mx[0] - mn[0], mx[1] - mn[1],
                                     mx[2] - mn[2] });
    if (!(extent > 1e-12) || !std::isfinite(extent))
        return failResult(QStringLiteral("SkinTokens: degenerate mesh bounds."));
    const double halfRange = 0.5 * (layout.rangeHi - layout.rangeLo);
    const double scale = 2.0 * halfRange / extent;
    const double mid[3] = { (mn[0] + mx[0]) * 0.5, (mn[1] + mx[1]) * 0.5,
                            (mn[2] + mx[2]) * 0.5 };
    const double centre = 0.5 * (layout.rangeLo + layout.rangeHi);
    auto normPt = [&](double x, double y, double z, float out[3]) {
        out[0] = float((x - mid[0]) * scale + centre);
        out[1] = float((y - mid[1]) * scale + centre);
        out[2] = float((z - mid[2]) * scale + centre);
    };

    dbg("sampling surface");
    // ── Sample the surface ──────────────────────────────────────────
    std::vector<std::array<float, 3>> pts, nrm;
    sampleSurface(positions, vertexCount, indices, indexCount, N, pts, nrm);
    if (int(pts.size()) != N)
        return failResult(QStringLiteral("SkinTokens: sampling failed."));
    std::vector<std::array<float, 3>> ptsWorld = pts;   // for the transfer
    for (auto& p : pts) {
        float o[3];
        normPt(p[0], p[1], p[2], o);
        p = { o[0], o[1], o[2] };
    }

    // ── Normalise the topology + tokenize the skeleton ─────────────
    // Real skeletons arrive in creation order with possibly MANY
    // roots (prop/attachment bones) — the token stream needs one
    // root and parent-first DFS order. Re-parent extra roots to the
    // first root and DFS-reorder; `order[k]` maps stream position k
    // back to the caller's joint index for the output columns.
    std::vector<int> order;
    {
        std::vector<std::vector<int>> children(joints.size());
        int firstRoot = -1;
        bool valid = true;
        for (int i = 0; i < J; ++i) {
            const int p = joints[std::size_t(i)].parent;
            if (p < 0) {
                if (firstRoot < 0) firstRoot = i;
                else children[std::size_t(firstRoot)].push_back(i);
            } else if (p < J && p != i) {
                children[std::size_t(p)].push_back(i);
            } else {
                valid = false;
            }
        }
        if (firstRoot < 0 || !valid)
            return failResult(QStringLiteral(
                "SkinTokens: invalid joint hierarchy."));
        order.reserve(std::size_t(J));
        std::vector<int> stack = { firstRoot };
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            order.push_back(i);
            const auto& c = children[std::size_t(i)];
            for (auto it = c.rbegin(); it != c.rend(); ++it)
                stack.push_back(*it);
        }
        if (int(order.size()) != J)
            return failResult(QStringLiteral(
                "SkinTokens: joint hierarchy contains a cycle."));
    }
    std::vector<int> oldToNew(std::size_t(J), -1);
    for (int k = 0; k < J; ++k) oldToNew[std::size_t(order[k])] = k;

    std::vector<Joint> normJoints(static_cast<std::size_t>(J));
    for (int k = 0; k < J; ++k) {
        const Joint& src = joints[std::size_t(order[k])];
        Joint& dst = normJoints[std::size_t(k)];
        float o[3];
        normPt(src.pos[0], src.pos[1], src.pos[2], o);
        dst.pos = { o[0], o[1], o[2] };
        if (k == 0) {
            dst.parent = -1;
        } else {
            const int p = src.parent < 0 ? order[0] : src.parent;
            dst.parent = oldToNew[std::size_t(p)];
        }
    }
    const std::vector<std::int64_t> skelIds =
        tokenizeSkeleton(normJoints, layout);
    if (skelIds.empty())
        return failResult(QStringLiteral(
            "SkinTokens: joints are not in a valid parent-first order."));

    dbg("opening ONNX env");
    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_skintokens");
        Ort::SessionOptions so;
        // hardware_concurrency() may legally return 0 — guard before the -1
        // (0u - 1 underflows to a UINT_MAX-sized thread pool request).
        const unsigned hc = std::thread::hardware_concurrency();
        so.SetIntraOpNumThreads(hc > 1 ? static_cast<int>(hc - 1) : 1);
        // Don't busy-spin the pool between ops — spinning starves the GUI
        // render loop even though the compute runs on a worker thread.
        so.AddConfigEntry("session.intra_op.allow_spinning", "0");
        auto openSession = [&](const char* file) -> Ort::Session {
            const QString p = modelDir() + QLatin1Char('/')
                              + QLatin1String(file);
#ifdef _WIN32
            const std::wstring wp = p.toStdWString();
            return Ort::Session(env, wp.c_str(), so);
#else
            const std::string sp = p.toStdString();
            return Ort::Session(env, sp.c_str(), so);
#endif
        };
        Ort::AllocatorWithDefaultOptions alloc;
        Ort::MemoryInfo mem =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        // Generic single-shot run helper: fixed input tensors → the
        // first float output (copied out).
        auto runSimple = [&](Ort::Session& s,
                             std::vector<Ort::Value>& inputs,
                             std::vector<int64_t>& outShape)
            -> std::vector<float> {
            const size_t nIn = s.GetInputCount();
            std::vector<Ort::AllocatedStringPtr> inHold;
            std::vector<const char*> inNames;
            for (size_t i = 0; i < nIn; ++i) {
                inHold.push_back(s.GetInputNameAllocated(i, alloc));
                inNames.push_back(inHold.back().get());
            }
            const size_t nOut = s.GetOutputCount();
            std::vector<Ort::AllocatedStringPtr> outHold;
            std::vector<const char*> outNames;
            for (size_t i = 0; i < nOut; ++i) {
                outHold.push_back(s.GetOutputNameAllocated(i, alloc));
                outNames.push_back(outHold.back().get());
            }
            auto outs = s.Run(Ort::RunOptions{nullptr}, inNames.data(),
                              inputs.data(), inputs.size(), outNames.data(),
                              outNames.size());
            for (auto& o : outs) {
                if (!o.IsTensor()) continue;
                auto info = o.GetTensorTypeAndShapeInfo();
                if (info.GetElementType()
                    != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
                    continue;
                outShape = info.GetShape();
                const float* d = o.GetTensorData<float>();
                std::size_t count = 1;
                for (auto s2 : outShape) count *= std::size_t(s2);
                return std::vector<float>(d, d + count);
            }
            return {};
        };

        dbg("stage 1: mesh_cond");
        // ── (1) mesh_cond ───────────────────────────────────────────
        std::vector<float> flatPts(std::size_t(N) * 3),
            flatNrm(std::size_t(N) * 3);
        for (int i = 0; i < N; ++i)
            for (int a = 0; a < 3; ++a) {
                flatPts[3 * i + a] = pts[i][a];
                flatNrm[3 * i + a] = nrm[i][a];
            }
        std::vector<float> condEmbeds;
        std::vector<int64_t> condShape;
        {
            Ort::Session s = openSession("mesh_cond.onnx");
            const int64_t shp[3] = { 1, N, 3 };
            std::vector<Ort::Value> ins;
            ins.push_back(Ort::Value::CreateTensor<float>(
                mem, flatPts.data(), flatPts.size(), shp, 3));
            ins.push_back(Ort::Value::CreateTensor<float>(
                mem, flatNrm.data(), flatNrm.size(), shp, 3));
            condEmbeds = runSimple(s, ins, condShape);
        }
        if (condEmbeds.empty() || condShape.size() != 3)
            return failResult(QStringLiteral(
                "SkinTokens: mesh conditioning failed."));
        const int64_t condLen = condShape[1];
        const int64_t hidden  = condShape[2];

        dbg("stage 2: vae_cond");
        // ── (2) vae_cond ────────────────────────────────────────────
        std::vector<float> vaeCondIn(std::size_t(N) * 6);
        for (int i = 0; i < N; ++i) {
            for (int a = 0; a < 3; ++a) {
                vaeCondIn[6 * i + a]     = pts[i][a];
                vaeCondIn[6 * i + 3 + a] = nrm[i][a];
            }
        }
        std::vector<float> condLatents;
        std::vector<int64_t> latShape;
        {
            Ort::Session s = openSession("vae_cond.onnx");
            const int64_t shp[3] = { 1, N, 6 };
            std::vector<Ort::Value> ins;
            ins.push_back(Ort::Value::CreateTensor<float>(
                mem, vaeCondIn.data(), vaeCondIn.size(), shp, 3));
            condLatents = runSimple(s, ins, latShape);
        }
        if (condLatents.empty() || latShape.size() != 3)
            return failResult(QStringLiteral(
                "SkinTokens: VAE conditioning failed."));

        dbg("stage 3: embed skeleton");
        // ── (3) embed the skeleton ids ──────────────────────────────
        std::vector<float> skelEmbeds;
        {
            Ort::Session s = openSession("embed.onnx");
            std::vector<std::int64_t> ids = skelIds;
            const int64_t shp[2] = { 1, int64_t(ids.size()) };
            std::vector<Ort::Value> ins;
            ins.push_back(Ort::Value::CreateTensor<int64_t>(
                mem, ids.data(), ids.size(), shp, 2));
            std::vector<int64_t> eshape;
            skelEmbeds = runSimple(s, ins, eshape);
            if (skelEmbeds.empty() || eshape.size() != 3
                || eshape[2] != hidden)
                return failResult(QStringLiteral(
                    "SkinTokens: token embedding failed."));
        }

        dbg("stage 4: decoder open");
        // ── (4) decoder prefix + greedy skin decode ─────────────────
        Ort::Session dec   = openSession("decoder.onnx");
        dbg("stage 4a: decoder session created");
        Ort::Session embed = openSession("embed.onnx");
        dbg("stage 4a2: embed session created");

        const size_t decIn = dec.GetInputCount();
        std::vector<Ort::AllocatedStringPtr> dInHold;
        std::vector<std::string> dInNames;
        int embIdx = -1;
        std::vector<int> pastIdx;
        for (size_t i = 0; i < decIn; ++i) {
            dInHold.push_back(dec.GetInputNameAllocated(i, alloc));
            dInNames.push_back(dInHold.back().get());
            const std::string& nm = dInNames.back();
            if (nm.find("embed") != std::string::npos && embIdx < 0)
                embIdx = int(i);
            else if (nm.find("past") != std::string::npos)
                pastIdx.push_back(int(i));
        }
        const size_t decOut = dec.GetOutputCount();
        std::vector<Ort::AllocatedStringPtr> dOutHold;
        std::vector<std::string> dOutNames;
        int logitsIdx = -1;
        std::vector<int> presentIdx;
        for (size_t i = 0; i < decOut; ++i) {
            dOutHold.push_back(dec.GetOutputNameAllocated(i, alloc));
            dOutNames.push_back(dOutHold.back().get());
            const std::string& nm = dOutNames.back();
            if (nm.find("present") != std::string::npos)
                presentIdx.push_back(int(i));
            else if (nm.find("logits") != std::string::npos)
                logitsIdx = int(i);
        }
        if (embIdx < 0 || logitsIdx < 0 || pastIdx.empty()
            || presentIdx.size() != pastIdx.size())
            return failResult(QStringLiteral(
                "SkinTokens: unexpected decoder I/O layout."));

        dbg("stage 4a3: decoder I/O discovered");
        // KV-cache geometry from the declared past input shape
        // [1, KV, seq, Dh] (seq dynamic). NB: GetTensorTypeAndShapeInfo()
        // returns a NON-OWNING view into the TypeInfo — the TypeInfo must
        // outlive it (chaining off the temporary reads freed memory).
        Ort::TypeInfo pastTypeInfo =
            dec.GetInputTypeInfo(std::size_t(pastIdx[0]));
        auto pastInfo  = pastTypeInfo.GetTensorTypeAndShapeInfo();
        auto pastShape = pastInfo.GetShape();
        if (pastShape.size() != 4)
            return failResult(QStringLiteral(
                "SkinTokens: unexpected KV-cache rank."));
        const int64_t numKv   = pastShape[1] > 0 ? pastShape[1] : 8;
        const int64_t headDim = pastShape[3] > 0 ? pastShape[3] : 128;
        const size_t  layers  = pastIdx.size() / 2;

        // Cache buffers (past for all layers; grown per step).
        std::vector<std::vector<float>> cache(pastIdx.size());
        int64_t cacheLen = 0;

        const int totalSteps = J * tokensPerSkin;
        std::vector<std::int64_t> skinIds;
        skinIds.reserve(std::size_t(totalSteps));

        // One decoder invocation over `T` embedding steps.
        auto runDecoder = [&](const std::vector<float>& embeds, int64_t T)
            -> std::vector<float> {
            std::vector<Ort::Value> ins;
            ins.reserve(decIn);
            std::vector<float> embCopy = embeds;
            std::vector<std::vector<float>> pastCopies(pastIdx.size());
            // Build in declared order.
            size_t pastCounter = 0;
            for (size_t i = 0; i < decIn; ++i) {
                if (int(i) == embIdx) {
                    const int64_t shp[3] = { 1, T, hidden };
                    ins.push_back(Ort::Value::CreateTensor<float>(
                        mem, embCopy.data(), embCopy.size(), shp, 3));
                } else {
                    auto& pc = pastCopies[pastCounter];
                    pc = cache[pastCounter];
                    // A zero-length past (step 0) must still hand ORT a
                    // VALID data pointer — data() of an empty vector is
                    // null and CreateTensor crashes on it.
                    if (pc.empty()) pc.reserve(1);
                    const int64_t shp[4] = { 1, numKv, cacheLen, headDim };
                    float* base = pc.empty()
                        ? reinterpret_cast<float*>(&pc)   // non-null, unused (0 elements)
                        : pc.data();
                    ins.push_back(Ort::Value::CreateTensor<float>(
                        mem, base, pc.size(), shp, 4));
                    ++pastCounter;
                }
            }
            std::vector<const char*> inN, outN;
            for (auto& n : dInNames) inN.push_back(n.c_str());
            for (auto& n : dOutNames) outN.push_back(n.c_str());
            auto outs = dec.Run(Ort::RunOptions{nullptr}, inN.data(),
                                ins.data(), ins.size(), outN.data(),
                                outN.size());
            // Update the cache from the presents.
            for (size_t p = 0; p < presentIdx.size(); ++p) {
                auto& o = outs[std::size_t(presentIdx[p])];
                auto info = o.GetTensorTypeAndShapeInfo();
                const auto shp = info.GetShape();
                const float* d = o.GetTensorData<float>();
                std::size_t cnt = 1;
                for (auto v : shp) cnt *= std::size_t(v);
                cache[p].assign(d, d + cnt);
            }
            cacheLen += T;
            // Copy out the last-step logits row.
            auto& lo = outs[std::size_t(logitsIdx)];
            auto li = lo.GetTensorTypeAndShapeInfo();
            const auto ls = li.GetShape();     // [1, T, V]
            const float* ld = lo.GetTensorData<float>();
            const int64_t V = ls[2];
            const float* last = ld + std::size_t(ls[1] - 1) * V;
            return std::vector<float>(last, last + V);
        };

        dbg("stage 4b: prefix pass");
        // Prefix: [mesh_cond | skeleton embeds] in one pass.
        std::vector<float> prefix;
        prefix.reserve(condEmbeds.size() + skelEmbeds.size());
        prefix.insert(prefix.end(), condEmbeds.begin(), condEmbeds.end());
        prefix.insert(prefix.end(), skelEmbeds.begin(), skelEmbeds.end());
        std::vector<float> logits =
            runDecoder(prefix, condLen + int64_t(skelIds.size()));

        // Greedy skin decode: only ids in
        // [tokVocab, tokVocab + fsqVocab) are legal until all
        // J*tokensPerSkin are emitted (VocabSwitchingLogitsProcessor
        // semantics).
        const int skinLo = tokVocab;
        const int skinHi = std::min<int>(tokVocab + fsqVocab, int(fullVocab));
        for (int step = 0; step < totalSteps; ++step) {
            if (progress && !progress(step, totalSteps))
                return failResult(QStringLiteral("cancelled"));
            int best = skinLo;
            float bestV = -std::numeric_limits<float>::infinity();
            for (int t = skinLo; t < skinHi && t < int(logits.size()); ++t) {
                if (logits[std::size_t(t)] > bestV) {
                    bestV = logits[std::size_t(t)];
                    best = t;
                }
            }
            skinIds.push_back(best);
            if (step + 1 == totalSteps) {
                if (!qEnvironmentVariableIsEmpty("QTMESH_SKINTOKENS_DEBUG")) {
                    std::set<std::int64_t> distinct(skinIds.begin(), skinIds.end());
                    fprintf(stderr, "[skintokens] decode done: %zu ids, %zu distinct, first=[",
                            skinIds.size(), distinct.size());
                    for (int i = 0; i < 12 && i < int(skinIds.size()); ++i)
                        fprintf(stderr, "%lld ", (long long)skinIds[size_t(i)]);
                    fprintf(stderr, "]\n");
                    fflush(stderr);
                }
                break;
            }
            // Embed the new token and step the decoder.
            std::vector<std::int64_t> one = { best };
            const int64_t shp[2] = { 1, 1 };
            std::vector<Ort::Value> ins;
            ins.push_back(Ort::Value::CreateTensor<int64_t>(
                mem, one.data(), 1, shp, 2));
            std::vector<int64_t> eshape;
            std::vector<float> e = runSimple(embed, ins, eshape);
            if (e.empty())
                return failResult(QStringLiteral(
                    "SkinTokens: token embedding failed mid-decode."));
            logits = runDecoder(e, 1);
        }

        dbg("stage 5: per-joint skin decode");
        // ── (5) per-joint skin decode ───────────────────────────────
        Ort::Session skinDec = openSession("skin_decode.onnx");
        std::vector<float> sampleWeights(std::size_t(N) * J, 0.f);
        for (int j = 0; j < J; ++j) {
            if (progress && !progress(totalSteps + j, totalSteps + J))
                return failResult(QStringLiteral("cancelled"));
            std::vector<std::int64_t> ids(static_cast<std::size_t>(tokensPerSkin));
            for (int t = 0; t < tokensPerSkin; ++t)
                ids[std::size_t(t)] =
                    skinIds[std::size_t(j) * tokensPerSkin + t] - tokVocab;
            const int64_t idShp[2] = { 1, tokensPerSkin };
            const int64_t cShp[3] = { 1, N, 6 };
            std::vector<Ort::Value> ins;
            ins.push_back(Ort::Value::CreateTensor<int64_t>(
                mem, ids.data(), ids.size(), idShp, 2));
            ins.push_back(Ort::Value::CreateTensor<float>(
                mem, vaeCondIn.data(), vaeCondIn.size(), cShp, 3));
            ins.push_back(Ort::Value::CreateTensor<float>(
                mem, condLatents.data(), condLatents.size(),
                latShape.data(), latShape.size()));
            std::vector<int64_t> wShape;
            const std::vector<float> w = runSimple(skinDec, ins, wShape);
            if (int(w.size()) < N)
                return failResult(QStringLiteral(
                    "SkinTokens: skin decode failed for joint %1.").arg(j));
            // Stream joint j ↔ caller joint order[j]: write the
            // column in CALLER space so the output indices need no
            // further mapping.
            const int callerJ = order[std::size_t(j)];
            for (int s = 0; s < N; ++s)
                sampleWeights[std::size_t(s) * J + callerJ]
                    = w[std::size_t(s)];
        }

        dbg("stage 6: transfer");
        // ── (6) transfer to full-res vertices ───────────────────────
        Result res;
        transferWeights(positions, vertexCount, ptsWorld, sampleWeights, J,
                        opts.maxInfluencesPerVertex, res.weights);
        res.ok = true;
        return res;
    } catch (const Ort::Exception& e) {
        return failResult(QStringLiteral("SkinTokens ONNX error: %1")
                              .arg(QString::fromUtf8(e.what())));
    } catch (const std::exception& e) {
        return failResult(QStringLiteral("SkinTokens error: %1")
                              .arg(QString::fromUtf8(e.what())));
    }
}

#endif // ENABLE_ONNX
