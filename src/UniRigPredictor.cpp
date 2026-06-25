#include "UniRigPredictor.h"
#include "ModelDownloader.h"

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {
// Both files are hosted alongside the #404 PBR/upscale ONNX models. The UniRig
// export is the encoder (Michelangelo SAL perceiver) + the AR decoder.
constexpr const char* kEncoderFile = "encoder.onnx";
constexpr const char* kDecoderFile = "decoder.onnx";
constexpr const char* kEmbedFile   = "embed.onnx";
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/unirig/";
constexpr const char* kBaseUrlSettingsKey = "ai/unirigModelBaseUrl";
constexpr const char* kEncoderLabel = "UniRig encoder model";
constexpr const char* kDecoderLabel = "UniRig decoder model";
constexpr const char* kEmbedLabel   = "UniRig embed model";

// cls token id for articulation-xl (the released checkpoint's training class).
// Seed = [latents ; embed([bos, cls])]  (matches UniRigAR.generate).
constexpr int kClsArticulationXL = 266;

// ---------------------------------------------------------------------------
// TOKENIZER CONSTANTS — EXACT replica of UniRig's
//   configs/tokenizer/tokenizer_parts_articulationxl_256.yaml
//   src/tokenizer/tokenizer_part.py
// The vocabulary id layout (offsets accumulate). DO NOT REORDER — every id
// below is load-bearing for both the FSM validity mask (decode) and the
// detokenizer FSM (decode → joints).
//
//   ids [0 .. 255]                 = discretized coordinate bins
//   offset = 256
//   token_id_branch = 256
//   token_id_bos    = 257
//   token_id_eos    = 258
//   token_id_pad    = 259
//   offset = 260
//   token_id_spring = 260          (a "part" token meaning None)
//   offset = 261
//   parts: body -> 261, hand -> 262     (parts_token_id {body:0,hand:1} + offset 261)
//   offset = 263
//   token_id_cls_none = 263
//   offset = 264
//   cls: vroid -> 264, mixamo -> 265, articulationxl -> 266   (+ offset 264)
//   vocab_size = 267
// ---------------------------------------------------------------------------
constexpr int kNumDiscrete   = 256;            // coordinate bins [0..255]
constexpr double kRangeLo    = -1.0;           // continuous_range = [-1, 1]
constexpr double kRangeHi    =  1.0;

constexpr int kTokBranch     = 256;
constexpr int kTokBos        = 257;
constexpr int kTokEos        = 258;
constexpr int kTokPad        = 259;
constexpr int kTokSpring     = 260;            // "part" None
constexpr int kTokPartBody   = 261;
constexpr int kTokPartHand   = 262;
constexpr int kTokClsNone    = 263;
constexpr int kTokClsVroid   = 264;
constexpr int kTokClsMixamo  = 265;
constexpr int kTokClsArtXL   = 266;
constexpr int kVocabSize     = 267;

// Surface sampling budget for inference (UniRig reference: num_samples=65536,
// vertex_samples=8192). We sample up to kNumSamples surface points; if the mesh
// has no faces we fall back to the vertices (capped) with estimated normals.
constexpr int kNumSamples    = 65536;
constexpr int kMaxNewTokens  = 2048;           // generation_config max_new_tokens

UniRigPredictor::Result failResult(const QString& why)
{
    UniRigPredictor::Result r;
    r.ok = false;
    r.error = why;
    return r;
}

// Rotate a point so the chosen up axis maps to +Y (UniRig is trained +Y-up),
// and the inverse swap. We only ever swap axes (no shear).
inline std::array<double,3> toModelUp(const std::array<double,3>& p, int up)
{
    if (up == 1) return p;                       // already +Y
    if (up == 0) return { p[1], p[0], p[2] };    // X-up  → swap X/Y
    return { p[0], p[2], p[1] };                 // Z-up  → swap Y/Z
}
inline std::array<double,3> fromModelUp(const std::array<double,3>& p, int up)
{
    if (up == 1) return p;
    if (up == 0) return { p[1], p[0], p[2] };    // inverse of X-up swap
    return { p[0], p[2], p[1] };                 // inverse of Z-up swap
}

QString jointName(int i, int parent)
{
    return parent < 0 ? QStringLiteral("root")
                      : QStringLiteral("joint_%1").arg(i);
}

} // namespace

#ifdef ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#ifdef __APPLE__
#include <unordered_map>
#endif
#endif

UniRigPredictor::Options::Options() = default;

// --- Pure-data tokenizer helpers (no ONNX) — exercised by predict() AND tests.
double UniRigPredictor::undiscretize(int bin)
{
    const double f = (static_cast<double>(bin) + 0.5) / static_cast<double>(kNumDiscrete);
    return f * (kRangeHi - kRangeLo) + kRangeLo;
}

UniRigPredictor::Result UniRigPredictor::detokenize(
        const std::vector<int>& idsIn, double scale,
        const std::array<double, 3>& centre)
{
    // Strip leading BOS / trailing PAD; drop the terminal EOS.
    std::vector<int> ids = idsIn;
    size_t begin = 0;
    if (!ids.empty() && ids.front() == kTokBos) begin = 1;
    size_t end = ids.size();
    while (end > begin && ids[end-1] == kTokPad) --end;
    if (end <= begin) return failResult(QStringLiteral("UniRig: empty token stream."));
    // The last real token MUST be EOS (matches the reference tokenizer, which
    // raises "last token is not eos"). Drop it before the walk.
    if (ids[end-1] != kTokEos)
        return failResult(QStringLiteral("UniRig: token stream does not end with EOS."));
    --end;

    struct DJoint { std::array<double,3> pos; std::array<double,3> parentPos; bool isRoot; };
    std::vector<DJoint> djoints;
    bool isBranch = false;
    std::array<double,3> lastJoint = {0,0,0};
    bool haveLast = false;

    auto undiscJoint = [&](size_t at, std::array<double,3>& outp) -> bool {
        if (at + 3 > end) return false;
        for (int a = 0; a < 3; ++a) {
            const int bin = ids[at + a];
            if (bin < 0 || bin >= kNumDiscrete) return false;
            outp[a] = undiscretize(bin);
        }
        return true;
    };

    size_t i = begin;
    while (i < end) {
        const int id = ids[i];
        if (id >= 0 && id < kNumDiscrete) {
            std::array<double,3> joint{}, pjoint{};
            if (isBranch) {
                if (!undiscJoint(i, pjoint))    return failResult(QStringLiteral("UniRig: truncated branch parent coords."));
                if (!undiscJoint(i + 3, joint)) return failResult(QStringLiteral("UniRig: truncated branch joint coords."));
                i += 6;
            } else {
                if (!undiscJoint(i, joint))     return failResult(QStringLiteral("UniRig: truncated joint coords."));
                if (djoints.empty())            pjoint = joint;       // root self-parent
                else if (haveLast)              pjoint = lastJoint;
                else                            pjoint = joint;
                i += 3;
            }
            DJoint dj; dj.pos = joint; dj.parentPos = pjoint; dj.isRoot = djoints.empty();
            djoints.push_back(dj);
            lastJoint = joint; haveLast = true; isBranch = false;
        } else if (id == kTokBranch) {
            isBranch = true; haveLast = false; ++i;
        } else if (id == kTokSpring || id == kTokPartBody || id == kTokPartHand) {
            ++i;                                  // part token — no geometry effect
        } else if (id == kTokClsVroid || id == kTokClsMixamo || id == kTokClsArtXL) {
            ++i;                                  // class token — no geometry effect
        } else if (id == kTokClsNone) {
            ++i;
        } else {
            return failResult(QStringLiteral(
                "UniRig: detokenizer hit an unexpected token id %1.").arg(id));
        }
    }
    if (djoints.empty())
        return failResult(QStringLiteral("UniRig: decode produced no joints."));

    auto samePos = [](const std::array<double,3>& a, const std::array<double,3>& b) {
        return std::abs(a[0]-b[0]) < 1e-9 && std::abs(a[1]-b[1]) < 1e-9 && std::abs(a[2]-b[2]) < 1e-9;
    };

    Result r;
    r.joints.reserve(djoints.size());
    for (size_t j = 0; j < djoints.size(); ++j) {
        int parent = -1;
        if (!djoints[j].isRoot && !samePos(djoints[j].pos, djoints[j].parentPos)) {
            for (int k = static_cast<int>(j) - 1; k >= 0; --k)
                if (samePos(djoints[(size_t)k].pos, djoints[j].parentPos)) { parent = k; break; }
            // A non-root joint whose explicit parent triple matches no earlier
            // joint is a malformed decode — reject so the caller falls back
            // rather than silently emitting it as a spurious extra root.
            if (parent < 0)
                return failResult(QStringLiteral(
                    "UniRig: a branch parent did not match any earlier joint "
                    "(malformed decode)."));
        }
        std::array<double,3> p = djoints[j].pos;
        p = { p[0]*scale + centre[0], p[1]*scale + centre[1], p[2]*scale + centre[2] };
        Joint jt; jt.parent = parent; jt.pos = p;
        jt.name = jointName(static_cast<int>(j), parent);
        r.joints.push_back(std::move(jt));
    }

    // NOTE: joints are returned in EMISSION order, which is already a valid
    // parent-before-child topo order — every joint's parent (last_joint, or an
    // earlier joint via a branch's explicit parent triple) was emitted before
    // it, so parent index < own index by construction. We deliberately do NOT
    // reorder here: callers (and tests) rely on emission-order indexing, and
    // Ogre bone creation is happy with any order where parents precede children.
    r.ok = true;
    return r;
}

bool UniRigPredictor::isAvailable()
{
#ifdef ENABLE_ONNX
    return true;
#else
    return false;
#endif
}

QString UniRigPredictor::modelPath()
{
    return encoderModelPath();
}

QString UniRigPredictor::encoderModelPath()
{
    const QString dataPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataPath).filePath(
        QStringLiteral("ai_models/unirig/") + QString::fromLatin1(kEncoderFile));
}

QString UniRigPredictor::decoderModelPath()
{
    const QString dataPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataPath).filePath(
        QStringLiteral("ai_models/unirig/") + QString::fromLatin1(kDecoderFile));
}

QString UniRigPredictor::embedModelPath()
{
    const QString dataPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataPath).filePath(
        QStringLiteral("ai_models/unirig/") + QString::fromLatin1(kEmbedFile));
}

QString UniRigPredictor::ensureModelBlocking()
{
#ifndef ENABLE_ONNX
    // A non-ONNX build can't run the model — don't touch disk/network or kick
    // off a ~1.4 GB first-use download that would never be used.
    return {};
#else
    const QString enc = encoderModelPath();
    const QString dec = decoderModelPath();
    const QString emb = embedModelPath();
    if (QFileInfo::exists(enc) && QFileInfo::exists(dec) && QFileInfo::exists(emb))
        return enc;

    // Offline / test guard — never hit the network when set.
    if (!qEnvironmentVariableIsEmpty("QTMESH_UNIRIG_NO_DOWNLOAD"))
        return {};

    // Resolve the download base URL (QSettings override → env → default HF repo).
    QString base;
    {
        QSettings s;
        base = s.value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_UNIRIG_MODEL_BASE_URL");
            base = env.isEmpty() ? QString::fromLatin1(kDefaultModelBaseUrl)
                                 : QString::fromUtf8(env);
        }
    }
    if (base.isEmpty()) return {};
    if (!base.endsWith('/')) base += '/';

    auto* dl = ModelDownloader::instance();
    if (!dl) return {};

    // Download one file, blocking via a local event loop (same pattern as
    // AIAssistManager::ensureModelBlocking / RigNetPredictor), with a hard
    // timeout so a stalled connection can't hang the synchronous rig call.
    auto downloadOne = [&](const QString& fileName, const QString& dest,
                           const QString& label) -> bool {
        QDir().mkpath(QFileInfo(dest).absolutePath());
        const QString url = base + fileName;
        QEventLoop loop;
        bool ok = false, timedOut = false;
        auto onDone = QObject::connect(dl, &ModelDownloader::downloadCompleted, &loop,
            [&](const QString& name, const QString&) {
                if (name == label) { ok = true; loop.quit(); }
            });
        auto onErr = QObject::connect(dl, &ModelDownloader::downloadError, &loop,
            [&](const QString& name, const QString&) {
                if (name == label) { ok = false; loop.quit(); }
            });
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop,
            [&]() { timedOut = true; loop.quit(); });
        // 30 min — UniRig is large (~1.44 GB across the 3 files; the decoder
        // alone is 1.2 GB), so a generous cap for slow links; a truly dead
        // connection still can't hang the synchronous rig call forever.
        timeout.start(1800000);
        dl->startDownload(url, dest, label);
        loop.exec();
        QObject::disconnect(onDone);
        QObject::disconnect(onErr);
        if (timedOut) dl->cancelDownload();
        return ok && !timedOut && QFileInfo::exists(dest);
    };

    if (!QFileInfo::exists(enc) &&
        !downloadOne(QString::fromLatin1(kEncoderFile), enc,
                     QString::fromLatin1(kEncoderLabel)))
        return {};
    if (!QFileInfo::exists(dec) &&
        !downloadOne(QString::fromLatin1(kDecoderFile), dec,
                     QString::fromLatin1(kDecoderLabel)))
        return {};
    if (!QFileInfo::exists(emb) &&
        !downloadOne(QString::fromLatin1(kEmbedFile), emb,
                     QString::fromLatin1(kEmbedLabel)))
        return {};

    // ALL THREE must exist for success.
    return (QFileInfo::exists(enc) && QFileInfo::exists(dec)
            && QFileInfo::exists(emb)) ? enc : QString();
#endif  // ENABLE_ONNX
}

#ifndef ENABLE_ONNX

UniRigPredictor::Result UniRigPredictor::predict(
        const float*, int, const uint32_t*, int,
        const QString&, const QString&, const QString&, const Options&)
{
    return failResult(QStringLiteral(
        "UniRig needs an ONNX-enabled build — rebuild with -DENABLE_ONNX "
        "(falling back to the native template rig)."));
}

#else  // ENABLE_ONNX

namespace {

// ---------------------------------------------------------------------------
// Surface sampler. Picks up to `target` points uniformly over the triangle
// surface (area-weighted face selection + barycentric sampling) and computes
// the per-point normal from its source face. Falls back to mesh vertices (with
// origin-direction estimated normals) when there are no faces. All in the
// already-normalised [-1,1] space the caller passes in.
// ---------------------------------------------------------------------------
struct SampledCloud {
    std::vector<float> pts;     // [N*3]
    std::vector<float> nrm;     // [N*3]
    int count = 0;
};

inline void cross3(const double a[3], const double b[3], double out[3])
{
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}

SampledCloud sampleSurface(const std::vector<float>& nverts, int vertexCount,
                           const uint32_t* indices, int indexCount, int target)
{
    SampledCloud out;
    const int triCount = (indices && indexCount >= 3) ? (indexCount / 3) : 0;

    if (triCount == 0) {
        // No faces: fall back to the vertices (capped to `target`) with
        // estimated normals (the position direction from the centred origin —
        // a coarse but valid outward normal for a centred mesh).
        const int n = std::min(vertexCount, target);
        out.pts.reserve(static_cast<size_t>(n) * 3);
        out.nrm.reserve(static_cast<size_t>(n) * 3);
        for (int i = 0; i < n; ++i) {
            const double x = nverts[3*i+0], y = nverts[3*i+1], z = nverts[3*i+2];
            double len = std::sqrt(x*x + y*y + z*z);
            if (len < 1e-9) len = 1.0;
            out.pts.push_back(static_cast<float>(x));
            out.pts.push_back(static_cast<float>(y));
            out.pts.push_back(static_cast<float>(z));
            out.nrm.push_back(static_cast<float>(x/len));
            out.nrm.push_back(static_cast<float>(y/len));
            out.nrm.push_back(static_cast<float>(z/len));
        }
        out.count = n;
        return out;
    }

    // Area-weighted face CDF + per-face normal.
    std::vector<double> cdf(triCount);
    std::vector<std::array<double,3>> faceN(triCount);
    double areaSum = 0.0;
    for (int t = 0; t < triCount; ++t) {
        const uint32_t i0 = indices[3*t+0], i1 = indices[3*t+1], i2 = indices[3*t+2];
        if (i0 >= static_cast<uint32_t>(vertexCount) ||
            i1 >= static_cast<uint32_t>(vertexCount) ||
            i2 >= static_cast<uint32_t>(vertexCount)) {
            cdf[t] = areaSum; faceN[t] = {0,1,0}; continue;
        }
        double a[3] = { nverts[3*i1+0]-nverts[3*i0+0], nverts[3*i1+1]-nverts[3*i0+1], nverts[3*i1+2]-nverts[3*i0+2] };
        double b[3] = { nverts[3*i2+0]-nverts[3*i0+0], nverts[3*i2+1]-nverts[3*i0+1], nverts[3*i2+2]-nverts[3*i0+2] };
        double nrm[3]; cross3(a, b, nrm);
        const double mag = std::sqrt(nrm[0]*nrm[0] + nrm[1]*nrm[1] + nrm[2]*nrm[2]);
        const double area = 0.5 * mag;
        areaSum += area;
        cdf[t] = areaSum;
        if (mag > 1e-12) faceN[t] = { nrm[0]/mag, nrm[1]/mag, nrm[2]/mag };
        else             faceN[t] = { 0, 1, 0 };
    }
    if (areaSum < 1e-12) {       // degenerate — every face zero-area
        out.count = 0;
        return out;
    }

    // Deterministic RNG so the same mesh always samples the same cloud (matches
    // the greedy-decode determinism contract).
    std::mt19937 rng(0x5eed5eedu);
    std::uniform_real_distribution<double> uni(0.0, 1.0);

    out.pts.reserve(static_cast<size_t>(target) * 3);
    out.nrm.reserve(static_cast<size_t>(target) * 3);
    for (int s = 0; s < target; ++s) {
        // Area-weighted face pick via binary search over the CDF.
        const double r = uni(rng) * areaSum;
        int lo = 0, hi = triCount - 1;
        while (lo < hi) { const int mid = (lo+hi)/2; if (cdf[mid] < r) lo = mid+1; else hi = mid; }
        const int t = lo;
        const uint32_t i0 = indices[3*t+0], i1 = indices[3*t+1], i2 = indices[3*t+2];
        if (i0 >= static_cast<uint32_t>(vertexCount) ||
            i1 >= static_cast<uint32_t>(vertexCount) ||
            i2 >= static_cast<uint32_t>(vertexCount)) { --s; continue; }
        // Uniform barycentric sampling on the triangle.
        double u = uni(rng), v = uni(rng);
        if (u + v > 1.0) { u = 1.0 - u; v = 1.0 - v; }
        const double w = 1.0 - u - v;
        for (int a = 0; a < 3; ++a) {
            const double p = w*nverts[3*i0+a] + u*nverts[3*i1+a] + v*nverts[3*i2+a];
            out.pts.push_back(static_cast<float>(p));
            out.nrm.push_back(static_cast<float>(faceN[t][a]));
        }
    }
    out.count = target;
    return out;
}

} // namespace

UniRigPredictor::Result UniRigPredictor::predict(
        const float* positions, int vertexCount,
        const uint32_t* indices, int indexCount,
        const QString& encoderModelPath,
        const QString& decoderModelPath,
        const QString& embedModelPath,
        const Options& opts)
{
    if (!positions || vertexCount < 4)
        return failResult(QStringLiteral("UniRig: mesh has too few vertices."));
    if (!QFileInfo::exists(encoderModelPath))
        return failResult(QStringLiteral(
            "UniRig encoder model not available at '%1' — connect to the "
            "internet to download it on first use.").arg(encoderModelPath));
    if (!QFileInfo::exists(decoderModelPath))
        return failResult(QStringLiteral(
            "UniRig decoder model not available at '%1' — connect to the "
            "internet to download it on first use.").arg(decoderModelPath));
    if (!QFileInfo::exists(embedModelPath))
        return failResult(QStringLiteral(
            "UniRig embed model not available at '%1' — connect to the "
            "internet to download it on first use.").arg(embedModelPath));

    const int up = std::clamp(opts.upAxis, 0, 2);

    // --- (1a) Normalise vertices into a centred unit box [-1,1] (+Y up) ------
    // UniRig's tokenizer continuous_range is [-1,1]; we record centre + half
    // (the half-extent) to de-normalise predicted joints back to mesh-local.
    std::array<double,3> mn = { 1e30, 1e30, 1e30}, mx = {-1e30,-1e30,-1e30};
    for (int i = 0; i < vertexCount; ++i)
        for (int a = 0; a < 3; ++a) {
            const double v = positions[3*i + a];
            mn[a] = std::min(mn[a], v); mx[a] = std::max(mx[a], v);
        }
    const std::array<double,3> centre = {
        0.5*(mn[0]+mx[0]), 0.5*(mn[1]+mx[1]), 0.5*(mn[2]+mx[2]) };
    double maxExtent = 0.0;
    for (int a = 0; a < 3; ++a) maxExtent = std::max(maxExtent, mx[a]-mn[a]);
    if (maxExtent < 1e-9) return failResult(QStringLiteral("UniRig: degenerate mesh bounds."));
    // Largest dimension maps to [-1,1] → half-extent = maxExtent/2.
    const double half = 0.5 * maxExtent;
    const double inv  = 1.0 / half;

    std::vector<float> nverts(static_cast<size_t>(vertexCount) * 3);
    for (int i = 0; i < vertexCount; ++i) {
        std::array<double,3> p = {
            (positions[3*i+0] - centre[0]) * inv,
            (positions[3*i+1] - centre[1]) * inv,
            (positions[3*i+2] - centre[2]) * inv };
        p = toModelUp(p, up);
        nverts[3*i+0] = static_cast<float>(p[0]);
        nverts[3*i+1] = static_cast<float>(p[1]);
        nverts[3*i+2] = static_cast<float>(p[2]);
    }

    // --- (1b) Surface-sample up to kNumSamples points + normals --------------
    SampledCloud cloud = sampleSurface(nverts, vertexCount, indices, indexCount, kNumSamples);
    if (cloud.count < 1)
        return failResult(QStringLiteral("UniRig: failed to sample mesh surface."));

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_unirig");
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef __APPLE__
        try {
            std::unordered_map<std::string, std::string> coremlOpts;
            so.AppendExecutionProvider("CoreML", coremlOpts);
        } catch (const Ort::Exception&) {}
#endif

        auto openSession = [&](const QString& path) -> Ort::Session {
#ifdef _WIN32
            std::wstring wpath = path.toStdWString();
            return Ort::Session(env, wpath.c_str(), so);
#else
            const std::string p = path.toStdString();
            return Ort::Session(env, p.c_str(), so);
#endif
        };

        Ort::Session encoder = openSession(encoderModelPath);
        Ort::Session decoder = openSession(decoderModelPath);
        Ort::Session embed   = openSession(embedModelPath);
        Ort::AllocatorWithDefaultOptions alloc;
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        // embed.onnx: input_ids[1,S] (int64) -> token_embeds[1,S,hidden]. Turns
        // the seed tokens + each generated token into the inputs_embeds the
        // decoder consumes (the exported decoder takes embeddings, not ids —
        // UniRig conditions the LM by prepending the encoder latents as embeds).
        auto embedTokens = [&](const std::vector<int64_t>& ids) -> std::vector<float> {
            const int64_t shp[2] = {1, static_cast<int64_t>(ids.size())};
            Ort::Value in = Ort::Value::CreateTensor<int64_t>(
                mem, const_cast<int64_t*>(ids.data()), ids.size(), shp, 2);
            auto inName  = embed.GetInputNameAllocated(0, alloc);
            auto outName = embed.GetOutputNameAllocated(0, alloc);
            const char* inN[]  = { inName.get() };
            const char* outN[] = { outName.get() };
            auto out = embed.Run(Ort::RunOptions{nullptr}, inN, &in, 1, outN, 1);
            const float* d = out[0].GetTensorData<float>();
            const size_t cnt = (size_t)out[0].GetTensorTypeAndShapeInfo().GetElementCount();
            return std::vector<float>(d, d + cnt);
        };

        // =====================================================================
        // (2) ENCODER: pc[1,N,3] + feats[1,N,3] (normals) → latents prefix.
        //     Discover input/output names at runtime (don't hard-code the
        //     export's tensor naming): the features input has a name containing
        //     "feat"/"normal"; everything else is the point cloud.
        // =====================================================================
        const int64_t N = cloud.count;
        const int64_t pcShape[3]   = {1, N, 3};
        const int64_t featShape[3] = {1, N, 3};

        const size_t encInCount = encoder.GetInputCount();
        std::vector<Ort::AllocatedStringPtr> encInHolders;
        std::vector<const char*> encInNames;
        std::vector<Ort::Value> encInVals;
        bool sawFeat = false;
        for (size_t i = 0; i < encInCount; ++i) {
            auto h = encoder.GetInputNameAllocated(i, alloc);
            const std::string nm = h.get();
            encInHolders.push_back(std::move(h));
            encInNames.push_back(encInHolders.back().get());
            const bool isFeat = (nm.find("feat") != std::string::npos ||
                                 nm.find("normal") != std::string::npos);
            if (isFeat) {
                sawFeat = true;
                encInVals.push_back(Ort::Value::CreateTensor<float>(
                    mem, cloud.nrm.data(), cloud.nrm.size(), featShape, 3));
            } else {
                encInVals.push_back(Ort::Value::CreateTensor<float>(
                    mem, cloud.pts.data(), cloud.pts.size(), pcShape, 3));
            }
        }
        // Defensive: exactly two inputs, neither named "feat" → make the second
        // one the normals (typical positional [pc, feats] export).
        if (encInCount == 2 && encInVals.size() == 2 && !sawFeat) {
            encInVals[1] = Ort::Value::CreateTensor<float>(
                mem, cloud.nrm.data(), cloud.nrm.size(), featShape, 3);
        }
        if (encInVals.empty())
            return failResult(QStringLiteral("UniRig: encoder declares no inputs."));

        const size_t encOutCount = encoder.GetOutputCount();
        std::vector<Ort::AllocatedStringPtr> encOutHolders;
        std::vector<const char*> encOutNames;
        for (size_t i = 0; i < encOutCount; ++i) {
            auto h = encoder.GetOutputNameAllocated(i, alloc);
            encOutHolders.push_back(std::move(h));
            encOutNames.push_back(encOutHolders.back().get());
        }

        std::vector<Ort::Value> encOuts =
            encoder.Run(Ort::RunOptions{nullptr},
                        encInNames.data(), encInVals.data(), encInVals.size(),
                        encOutNames.data(), encOutNames.size());

        // The latent prefix: the first float output with rank>=2. Its shape is
        // [1, num_latents, hidden] (already projected to the decoder hidden by
        // the encoder graph's trailing nn.Linear). We feed it as the decoder's
        // `inputs_embeds` prefix.
        const float* latentData = nullptr;
        int64_t numLatents = 0, hidden = 0;
        for (size_t i = 0; i < encOuts.size(); ++i) {
            if (!encOuts[i].IsTensor()) continue;
            auto info = encOuts[i].GetTensorTypeAndShapeInfo();
            if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) continue;
            auto shape = info.GetShape();
            if (shape.size() >= 2) {
                hidden     = shape.back();
                numLatents = shape[shape.size() - 2];
                latentData = encOuts[i].GetTensorData<float>();
                break;
            }
        }
        if (!latentData || numLatents <= 0 || hidden <= 0)
            return failResult(QStringLiteral(
                "UniRig: encoder produced no latent prefix."));

        // Own the latent bytes (encOuts is reused/invalidated as we proceed).
        std::vector<float> latents(
            latentData, latentData + static_cast<size_t>(numLatents) * hidden);

        // =====================================================================
        // (3) GREEDY CONSTRAINED AUTOREGRESSIVE DECODE with a manual KV-cache.
        //
        // The reference repo uses beam search + sampling; we use deterministic
        // greedy decode constrained to the tokenizer FSM's next-possible-token
        // mask. The decoder graph is exported as a single-step CausalLM:
        //   inputs:
        //     * inputs_embeds [1,T,hidden]  — embeddings for THIS step
        //         (the latent prefix on step 0).
        //     * input_ids     [1,T]         — token ids for token steps (the
        //         graph owns its embedding table; we cannot embed in C++, so we
        //         REQUIRE this slot for the per-token loop).
        //     * past_key_values.* / past.*  — KV-cache from the previous step
        //         (empty, seq-len 0, on step 0).
        //   outputs:
        //     * logits  [1,T,vocab=267]
        //     * present.* / present_key_values.* — updated KV-cache (mapped 1:1
        //         to the matching past inputs by order).
        //
        // STEP 0 feeds the latent prefix as inputs_embeds + empty cache; we keep
        // its present cache and discard its logits (pure context). Then we seed
        // the stream with BOS(257) and loop: feed the LAST id via input_ids +
        // the cache, take argmax over VALID tokens (FSM mask), append, stop at
        // EOS(258) or kMaxNewTokens.
        // =====================================================================

        // ---- Discover decoder I/O once. ----------------------------------------
        const size_t decInCount = decoder.GetInputCount();
        // Ort::AllocatedStringPtr is a unique_ptr with a non-default-constructible
        // deleter, so the vectors can't be pre-sized — reserve + push_back.
        std::vector<Ort::AllocatedStringPtr> decInHolders; decInHolders.reserve(decInCount);
        std::vector<std::string> decInNames; decInNames.reserve(decInCount);
        int embedsInIdx = -1;        // inputs_embeds slot (latent prefix)
        int idsInIdx    = -1;        // input_ids slot (per-token loop)
        std::vector<int> pastInIdx;  // KV-cache input slots (order matters)
        for (size_t i = 0; i < decInCount; ++i) {
            decInHolders.push_back(decoder.GetInputNameAllocated(i, alloc));
            decInNames.push_back(decInHolders.back().get());
            const std::string& nm = decInNames.back();
            if (nm.find("embed") != std::string::npos && embedsInIdx < 0) embedsInIdx = (int)i;
            else if (nm.find("input_ids") != std::string::npos && idsInIdx < 0) idsInIdx = (int)i;
            else if (nm.find("past") != std::string::npos) pastInIdx.push_back((int)i);
        }

        const size_t decOutCount = decoder.GetOutputCount();
        std::vector<Ort::AllocatedStringPtr> decOutHolders; decOutHolders.reserve(decOutCount);
        std::vector<std::string> decOutNames; decOutNames.reserve(decOutCount);
        std::vector<int> presentOutIdx;
        for (size_t i = 0; i < decOutCount; ++i) {
            decOutHolders.push_back(decoder.GetOutputNameAllocated(i, alloc));
            decOutNames.push_back(decOutHolders.back().get());
            const std::string& nm = decOutNames.back();
            if (nm.find("present") != std::string::npos) presentOutIdx.push_back((int)i);
        }

        if (embedsInIdx < 0)
            return failResult(QStringLiteral(
                "UniRig: decoder lacks an inputs_embeds slot for the latent prefix."));
        (void)idsInIdx;   // decoder takes inputs_embeds only; tokens are embedded
                          // via embed.onnx (no input_ids slot in the export).

        // ---- FSM VALIDITY MASK + transition. -----------------------------------
        // The tokenizer is a finite-state machine over the token stream. At each
        // step only a subset of the 267 vocab ids is legal; we mask the logits to
        // those before argmax so the greedy decode can never emit a stream the
        // detokenizer would reject. Relevant states for the skeleton stream:
        //   - After BOS(257): optional cls (264/265/266) or cls_none(263), then
        //     optional part (260/261/262), then the joint stream.
        //   - A "joint record" is either 3 coord bins (a child of the previous
        //     joint), or BRANCH(256) + 6 bins (an explicit (parent3, joint3)).
        //   - The first joint is the root (3 bins, self-parent).
        //   - EOS(258) is legal only at a RECORD BOUNDARY after >=1 joint.
        //
        // State: coordsRemaining = coord bins still owed in the current record;
        // inBranch = filling a branch's 6-tuple; haveAnyJoint; phase.
        enum class Phase { AfterBos, AfterCls, AfterPart, RecordBoundary, InCoords };
        Phase phase = Phase::AfterBos;
        int   coordsRemaining = 0;
        bool  inBranch = false;
        bool  haveAnyJoint = false;

        auto buildValidMask = [&](std::vector<char>& valid) {
            valid.assign(kVocabSize, 0);
            if (coordsRemaining > 0) {
                // Mid-record: only coordinate bins [0..255] are legal.
                for (int t = 0; t < kNumDiscrete; ++t) valid[t] = 1;
                return;
            }
            switch (phase) {
            case Phase::AfterBos:
                // cls token, cls_none, part tokens, branch, or a coord bin
                // (the root joint) — all legal openers. (EOS illegal — no joints.)
                valid[kTokClsVroid] = valid[kTokClsMixamo] = valid[kTokClsArtXL] = 1;
                valid[kTokClsNone]  = 1;
                valid[kTokSpring]   = valid[kTokPartBody] = valid[kTokPartHand] = 1;
                valid[kTokBranch]   = 1;
                for (int t = 0; t < kNumDiscrete; ++t) valid[t] = 1;
                break;
            case Phase::AfterCls:
                // After a cls token: a part token, branch, or a coord bin (root).
                valid[kTokSpring]   = valid[kTokPartBody] = valid[kTokPartHand] = 1;
                valid[kTokBranch]   = 1;
                for (int t = 0; t < kNumDiscrete; ++t) valid[t] = 1;
                break;
            case Phase::AfterPart:
                // After a part token: branch or a coord bin (root joint).
                valid[kTokBranch] = 1;
                for (int t = 0; t < kNumDiscrete; ++t) valid[t] = 1;
                break;
            case Phase::RecordBoundary:
                // Between records: branch, a coord bin (continue chain), or EOS.
                valid[kTokBranch] = 1;
                for (int t = 0; t < kNumDiscrete; ++t) valid[t] = 1;
                if (haveAnyJoint) valid[kTokEos] = 1;
                break;
            case Phase::InCoords:
                for (int t = 0; t < kNumDiscrete; ++t) valid[t] = 1;
                break;
            }
        };

        // Advance the FSM after CONSUMING an emitted token. Returns false on an
        // illegal transition (should never trigger given the mask).
        auto advanceFsm = [&](int tok) -> bool {
            if (coordsRemaining > 0) {
                if (tok >= kNumDiscrete) return false;
                --coordsRemaining;
                if (coordsRemaining == 0) {
                    // Finished a record (3 or 6 coords) — a joint has landed.
                    haveAnyJoint = true;
                    inBranch = false;
                    phase = Phase::RecordBoundary;
                }
                return true;
            }
            if (tok < kNumDiscrete) {
                // Start a coord record. A branch owes 6 coords, a plain joint 3.
                // We've just consumed THIS first bin, so the remainder is 5 or 2.
                coordsRemaining = inBranch ? 5 : 2;
                phase = Phase::InCoords;
                return true;
            }
            switch (tok) {
            case kTokBranch:
                inBranch = true; coordsRemaining = 6; phase = Phase::InCoords; return true;
            case kTokClsVroid: case kTokClsMixamo: case kTokClsArtXL: case kTokClsNone:
                phase = Phase::AfterCls; return true;
            case kTokSpring: case kTokPartBody: case kTokPartHand:
                phase = Phase::AfterPart; return true;
            case kTokEos:
                return true;     // terminal
            default:
                return false;
            }
        };

        // ---- Manual KV-cache plumbing. -----------------------------------------
        // Ort::Value created from a pointer does NOT own its data, so we keep the
        // backing vectors alive in kvStorage/kvShapes and hand the cache between
        // steps. kvCache holds the current "past" values (parallel to pastInIdx).
        std::vector<std::vector<float>>   kvStorage;
        std::vector<std::vector<int64_t>> kvShapes;
        std::vector<Ort::Value> kvCache;
        kvStorage.reserve(presentOutIdx.size() * 2 + pastInIdx.size() + 8);
        kvShapes.reserve(presentOutIdx.size() * 2 + pastInIdx.size() + 8);

        // Append an owned copy of a present KV tensor to `dst`.
        auto kvStorePushFn = [&](const std::vector<int64_t>& shp, const float* src,
                                 size_t cnt, std::vector<Ort::Value>& dst) {
            kvShapes.push_back(shp);
            kvStorage.emplace_back(src, src + cnt);
            dst.push_back(Ort::Value::CreateTensor<float>(
                mem, kvStorage.back().data(), kvStorage.back().size(),
                kvShapes.back().data(), kvShapes.back().size()));
        };

        // Build the initial EMPTY past cache (one zero-length tensor per past
        // input, shaped to match the declared input rank with dynamic dims 0).
        for (int inIdx : pastInIdx) {
            Ort::TypeInfo ti = decoder.GetInputTypeInfo((size_t)inIdx);
            auto tsi = ti.GetTensorTypeAndShapeInfo();
            auto shape = tsi.GetShape();          // dynamic dims are -1
            std::vector<int64_t> dims(shape.size(), 0);
            for (size_t d = 0; d < shape.size(); ++d)
                dims[d] = (shape[d] > 0) ? shape[d] : 0;   // dynamic / seq → 0 (empty)
            kvShapes.push_back(dims);
            kvStorage.emplace_back();             // empty backing
            kvCache.push_back(Ort::Value::CreateTensor<float>(
                mem, kvStorage.back().data(), 0,
                kvShapes.back().data(), kvShapes.back().size()));
        }

        // Run one decoder step. `inputVal` goes into the embeds slot (isEmbeds)
        // or the ids slot. The KV-cache past tensors are appended automatically
        // and refreshed from the present outputs. Returns the step's outputs
        // (valid until the NEXT call, since the cache is deep-copied out here).
        // Always feeds inputs_embeds (the only non-cache decoder input).
        auto stepDecoder = [&](Ort::Value embedsVal) -> std::vector<Ort::Value> {
            std::vector<const char*> inN;
            std::vector<Ort::Value>  inV;
            inN.push_back(decInNames[embedsInIdx].c_str());
            inV.push_back(std::move(embedsVal));
            for (size_t k = 0; k < pastInIdx.size(); ++k) {
                inN.push_back(decInNames[pastInIdx[k]].c_str());
                inV.push_back(std::move(kvCache[k]));
            }
            std::vector<const char*> outN;
            for (auto& s : decOutNames) outN.push_back(s.c_str());
            std::vector<Ort::Value> outVals =
                decoder.Run(Ort::RunOptions{nullptr},
                            inN.data(), inV.data(), inV.size(),
                            outN.data(), outN.size());
            // Refresh the KV-cache from the present outputs into owned storage,
            // 1:1 with the past inputs (only when counts line up — otherwise the
            // export is non-cached and we just re-run from scratch each step).
            if (!presentOutIdx.empty() && presentOutIdx.size() == pastInIdx.size()) {
                std::vector<Ort::Value> nextCache;
                for (int oi : presentOutIdx) {
                    auto info = outVals[(size_t)oi].GetTensorTypeAndShapeInfo();
                    auto shp  = info.GetShape();
                    const size_t cnt = (size_t)info.GetElementCount();
                    const float* src = outVals[(size_t)oi].GetTensorData<float>();
                    kvStorePushFn(shp, src, cnt, nextCache);
                }
                kvCache = std::move(nextCache);
            }
            return outVals;
        };

        // Pull the LAST-position logits row [vocab] from a decoder step's outputs.
        auto lastLogitsRow = [&](std::vector<Ort::Value>& outVals,
                                 const float** outRow) -> bool {
            for (size_t i = 0; i < outVals.size(); ++i) {
                if (!outVals[i].IsTensor()) continue;
                auto info = outVals[i].GetTensorTypeAndShapeInfo();
                if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) continue;
                auto shp = info.GetShape();
                if (!shp.empty() && shp.back() == kVocabSize) {
                    const int64_t seqT = (shp.size() >= 2) ? shp[shp.size()-2] : 1;
                    const float* logits = outVals[i].GetTensorData<float>();
                    *outRow = logits + static_cast<size_t>(seqT - 1) * kVocabSize;
                    return true;
                }
            }
            return false;
        };

        // ---- STEP 0: seed = [latents ; embed([bos, cls=articulation-xl])]. -----
        // UniRigAR.generate prepends the encoder latents to the embedded start
        // tokens, runs the LM, and decodes from the last position. We replicate
        // that: build the seed inputs_embeds, run once, and read its last row to
        // pick the first generated token.
        std::vector<int> tokens;          // decoded id stream (incl. the seed)
        tokens.reserve(kMaxNewTokens + 4);
        tokens.push_back(kTokBos);
        tokens.push_back(kClsArticulationXL);
        phase = Phase::AfterBos;
        coordsRemaining = 0; inBranch = false; haveAnyJoint = false;
        advanceFsm(kClsArticulationXL);   // bos is implicit; advance past cls → AfterCls

        const std::vector<float> seedTokEmb =
            embedTokens({ kTokBos, (int64_t)kClsArticulationXL });   // [1,2,hidden]
        std::vector<float> seed = latents;                            // copy [L*hidden]
        seed.insert(seed.end(), seedTokEmb.begin(), seedTokEmb.end());
        const int64_t seedLen = numLatents + 2;
        const int64_t seedShape[3] = {1, seedLen, hidden};
        std::vector<Ort::Value> outVals =
            stepDecoder(Ort::Value::CreateTensor<float>(
                mem, seed.data(), seed.size(), seedShape, 3));

        // ---- TOKEN LOOP (constrained greedy, KV-cached). -----------------------
        for (int gen = 0; gen < kMaxNewTokens; ++gen) {
            const float* row = nullptr;
            if (!lastLogitsRow(outVals, &row))
                return failResult(QStringLiteral(
                    "UniRig: decoder produced no [.,vocab=267] logits output."));

            std::vector<char> valid;
            buildValidMask(valid);
            int best = -1; float bestLogit = -std::numeric_limits<float>::infinity();
            for (int t = 0; t < kVocabSize; ++t) {
                if (valid[t] && row[t] > bestLogit) { bestLogit = row[t]; best = t; }
            }
            if (best < 0) {
                if (haveAnyJoint) { tokens.push_back(kTokEos); break; }
                return failResult(QStringLiteral(
                    "UniRig: constrained decode reached a dead state."));
            }
            tokens.push_back(best);
            if (best == kTokEos) break;
            if (!advanceFsm(best))
                return failResult(QStringLiteral(
                    "UniRig: decoder emitted a token the FSM rejected."));
            // Embed the just-emitted token and step the decoder for the next one.
            const std::vector<float> te = embedTokens({ (int64_t)best });  // [1,1,hidden]
            const int64_t stepShape[3] = {1, 1, hidden};
            std::vector<float> teBuf = te;   // own the storage across the call
            outVals = stepDecoder(Ort::Value::CreateTensor<float>(
                mem, teBuf.data(), teBuf.size(), stepShape, 3));
        }

        // =====================================================================
        // (4) DETOKENIZE — the pure-data FSM (the static detokenize() helper,
        // exercised here AND by unit tests). It de-normalizes joints with the
        // model-space scale (half) + centre; here we additionally undo the +Y
        // up-axis rotation that toModelUp applied to the input. (The static
        // works in the model's axis convention by contract.)
        // =====================================================================
        Result r = detokenize(tokens, half, centre);
        if (!r.ok) return r;
        for (auto& jt : r.joints) {
            std::array<double,3> local = { (jt.pos[0] - centre[0]) / (half > 1e-12 ? half : 1.0),
                                           (jt.pos[1] - centre[1]) / (half > 1e-12 ? half : 1.0),
                                           (jt.pos[2] - centre[2]) / (half > 1e-12 ? half : 1.0) };
            local = fromModelUp(local, up);
            jt.pos = { local[0]*half + centre[0], local[1]*half + centre[1], local[2]*half + centre[2] };
        }
        // Apply the coarse joint cap (orphan any now-dangling child).
        if (opts.maxJoints > 0 && static_cast<int>(r.joints.size()) > opts.maxJoints) {
            r.joints.resize(opts.maxJoints);
            for (auto& j : r.joints) if (j.parent >= opts.maxJoints) j.parent = -1;
        }
        return r;
    } catch (const Ort::Exception& e) {
        return failResult(QStringLiteral("UniRig ONNX error: %1")
            .arg(QString::fromUtf8(e.what())));
    } catch (const std::exception& e) {
        return failResult(QStringLiteral("UniRig error: %1")
            .arg(QString::fromUtf8(e.what())));
    }
}

#if 0  // ---- superseded inline detokenizer (now the static detokenize()) ----
        std::vector<int> ids = tokens;
        size_t begin = 0;
        if (!ids.empty() && ids.front() == kTokBos) begin = 1;       // strip BOS
        size_t end = ids.size();
        while (end > begin && ids[end-1] == kTokPad) --end;          // strip PAD
        if (end <= begin)
            return failResult(QStringLiteral("UniRig: empty token stream."));
        if (ids[end-1] == kTokEos) --end;                            // drop terminal EOS

        struct DJoint { std::array<double,3> pos; std::array<double,3> parentPos; bool isRoot; };
        std::vector<DJoint> djoints;
        bool isBranch = false;
        std::array<double,3> lastJoint = {0,0,0};
        bool haveLast = false;

        auto undiscJoint = [&](size_t at, std::array<double,3>& outp) -> bool {
            if (at + 3 > end) return false;
            for (int a = 0; a < 3; ++a) {
                const int bin = ids[at + a];
                if (bin < 0 || bin >= kNumDiscrete) return false;
                outp[a] = undiscretize(bin);
            }
            return true;
        };

        size_t i = begin;
        while (i < end) {
            const int id = ids[i];
            if (id >= 0 && id < kNumDiscrete) {
                std::array<double,3> joint{}, pjoint{};
                if (isBranch) {
                    // 6 bins: parent (3) then joint (3).
                    if (!undiscJoint(i, pjoint))    return failResult(QStringLiteral("UniRig: truncated branch parent coords."));
                    if (!undiscJoint(i + 3, joint)) return failResult(QStringLiteral("UniRig: truncated branch joint coords."));
                    i += 6;
                } else {
                    // 3 bins: the joint. Parent is self (root) or the last joint.
                    if (!undiscJoint(i, joint))     return failResult(QStringLiteral("UniRig: truncated joint coords."));
                    if (djoints.empty())            pjoint = joint;          // root self-parent
                    else if (haveLast)              pjoint = lastJoint;
                    else                            pjoint = joint;          // defensive
                    i += 3;
                }
                DJoint dj;
                dj.pos       = joint;
                dj.parentPos = pjoint;
                dj.isRoot    = djoints.empty();
                djoints.push_back(dj);
                lastJoint = joint; haveLast = true;
                isBranch  = false;
            } else if (id == kTokBranch) {
                isBranch = true; haveLast = false; ++i;
            } else if (id == kTokSpring || id == kTokPartBody || id == kTokPartHand) {
                // "part" token — recorded by the reference impl; no geometry
                // effect for the skeleton, so just skip it.
                ++i;
            } else if (id == kTokClsVroid || id == kTokClsMixamo || id == kTokClsArtXL) {
                // class token — skip (no geometry effect for our purposes).
                ++i;
            } else if (id == kTokClsNone) {
                ++i;
            } else {
                return failResult(QStringLiteral(
                    "UniRig: detokenizer hit an unexpected token id %1.").arg(id));
            }
        }

        if (djoints.empty())
            return failResult(QStringLiteral("UniRig: decode produced no joints."));

        // Resolve parent indices: a joint's parent is the nearest EARLIER joint
        // whose position == its parentPos (exact — both come from the same
        // 256-bin grid). The root's parentPos == its own pos → parent = -1.
        auto samePos = [](const std::array<double,3>& a, const std::array<double,3>& b) {
            return std::abs(a[0]-b[0]) < 1e-9 &&
                   std::abs(a[1]-b[1]) < 1e-9 &&
                   std::abs(a[2]-b[2]) < 1e-9;
        };

        Result r;
        r.joints.reserve(djoints.size());
        for (size_t j = 0; j < djoints.size(); ++j) {
            int parent = -1;
            if (!djoints[j].isRoot && !samePos(djoints[j].pos, djoints[j].parentPos)) {
                for (int k = static_cast<int>(j) - 1; k >= 0; --k) {
                    if (samePos(djoints[(size_t)k].pos, djoints[j].parentPos)) { parent = k; break; }
                }
            }
            // De-normalize from [-1,1] back to mesh-local (undo +Y rotation,
            // then * half + centre).
            std::array<double,3> p = djoints[j].pos;
            p = fromModelUp(p, up);
            p = { p[0]*half + centre[0], p[1]*half + centre[1], p[2]*half + centre[2] };
            Joint jt;
            jt.parent = parent;
            jt.pos    = p;
            jt.name   = jointName(static_cast<int>(j), parent);
            r.joints.push_back(std::move(jt));
        }

        // Enforce the coarse joint cap — truncate trailing joints, orphan any
        // now-dangling child to a root.
        if (opts.maxJoints > 0 && static_cast<int>(r.joints.size()) > opts.maxJoints) {
            r.joints.resize(opts.maxJoints);
            for (auto& j : r.joints)
                if (j.parent >= opts.maxJoints) j.parent = -1;
        }

        // Ensure parent-before-child ordering (Ogre bone creation needs a valid
        // topo order). Stable reorder: roots first, then DFS. (Same routine as
        // RigNetPredictor.)
        {
            const int n = static_cast<int>(r.joints.size());
            std::vector<std::vector<int>> kids(n);
            std::vector<int> roots;
            for (int k = 0; k < n; ++k) {
                if (r.joints[k].parent < 0) roots.push_back(k);
                else kids[r.joints[k].parent].push_back(k);
            }
            std::vector<int> order; order.reserve(n);
            std::vector<int> stack(roots.rbegin(), roots.rend());
            std::vector<bool> seen(n, false);
            while (!stack.empty()) {
                int cur = stack.back(); stack.pop_back();
                if (seen[cur]) continue;
                seen[cur] = true; order.push_back(cur);
                for (auto it = kids[cur].rbegin(); it != kids[cur].rend(); ++it)
                    stack.push_back(*it);
            }
            for (int k = 0; k < n; ++k) if (!seen[k]) order.push_back(k);
            if (static_cast<int>(order.size()) == n) {
                std::vector<int> remap(n);
                for (int newIdx = 0; newIdx < n; ++newIdx) remap[order[newIdx]] = newIdx;
                std::vector<Joint> reordered; reordered.reserve(n);
                for (int oldIdx : order) {
                    Joint jt = r.joints[oldIdx];
                    jt.parent = jt.parent < 0 ? -1 : remap[jt.parent];
                    reordered.push_back(std::move(jt));
                }
                r.joints = std::move(reordered);
            }
        }

        r.ok = true;
        return r;
    } catch (const Ort::Exception& e) {
        return failResult(QStringLiteral("UniRig ONNX error: %1")
            .arg(QString::fromUtf8(e.what())));
    } catch (const std::exception& e) {
        return failResult(QStringLiteral("UniRig error: %1")
            .arg(QString::fromUtf8(e.what())));
    }
}
#endif  // #if 0 (superseded inline detokenizer)

#endif  // ENABLE_ONNX
