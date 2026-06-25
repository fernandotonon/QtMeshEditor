#include "RigNetPredictor.h"
#include "ModelDownloader.h"

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace {
// Hosted alongside the #404 PBR/upscale ONNX models. The RigNet export is
// pending (see RigNetPredictor.h design contract); until it's uploaded the
// download 404s and AutoRig falls back to Pinocchio with a clear message.
constexpr const char* kRigNetModelFile = "rignet.onnx";
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/rignet/";
constexpr const char* kBaseUrlSettingsKey = "ai/rignetModelBaseUrl";
constexpr const char* kDownloadLabel = "RigNet skeleton model";
}

#ifdef ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#ifdef __APPLE__
#include <unordered_map>
#endif
#endif

namespace {

RigNetPredictor::Result failResult(const QString& why)
{
    RigNetPredictor::Result r;
    r.ok = false;
    r.error = why;
    return r;
}

// Rotate a point so the chosen up axis maps to +Y (RigNet is trained +Y-up),
// and the inverse. We only ever swap axes (no shear), so the inverse is the
// same swap applied again is NOT identity — keep an explicit pair.
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

// Synthesise a readable joint name. RigNet emits an unnamed joint graph; we
// label by index (the skeleton is still fully usable / exportable). A future
// pass could map common chains to humanoid names.
QString jointName(int i, int parent)
{
    return parent < 0 ? QStringLiteral("root")
                      : QStringLiteral("joint_%1").arg(i);
}

} // namespace

RigNetPredictor::Options::Options() = default;

bool RigNetPredictor::isAvailable()
{
#ifdef ENABLE_ONNX
    return true;
#else
    return false;
#endif
}

QString RigNetPredictor::modelPath()
{
    const QString dataPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataPath).filePath(
        QStringLiteral("ai_models/rignet/") + QString::fromLatin1(kRigNetModelFile));
}

QString RigNetPredictor::ensureModelBlocking()
{
    const QString dest = modelPath();
    if (QFileInfo::exists(dest))
        return dest;
    // Offline / test guard — never hit the network when set.
    if (!qEnvironmentVariableIsEmpty("QTMESH_RIGNET_NO_DOWNLOAD"))
        return {};

    // Resolve the download URL (QSettings override → env → default HF repo).
    QString base;
    {
        QSettings s;
        base = s.value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_RIGNET_MODEL_BASE_URL");
            base = env.isEmpty() ? QString::fromLatin1(kDefaultModelBaseUrl)
                                 : QString::fromUtf8(env);
        }
    }
    if (base.isEmpty()) return {};
    if (!base.endsWith('/')) base += '/';
    const QString url = base + QString::fromLatin1(kRigNetModelFile);

    auto* dl = ModelDownloader::instance();
    if (!dl) return {};
    QDir().mkpath(QFileInfo(dest).absolutePath());

    // Drive the async downloader with a local event loop (same pattern as
    // AIAssistManager::ensureModelBlocking), with a hard timeout so a stalled
    // connection can't hang the synchronous rig call.
    const QString label = QString::fromLatin1(kDownloadLabel);
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
    timeout.start(180000);   // 180s — RigNet is ~50 MB
    dl->startDownload(url, dest, label);
    loop.exec();
    QObject::disconnect(onDone);
    QObject::disconnect(onErr);
    if (timedOut) dl->cancelDownload();
    return (ok && !timedOut && QFileInfo::exists(dest)) ? dest : QString();
}

#ifndef ENABLE_ONNX

RigNetPredictor::Result RigNetPredictor::predict(
        const float*, int, const uint32_t*, int, const QString&, const Options&)
{
    return failResult(QStringLiteral(
        "RigNet needs an ONNX-enabled build — rebuild with -DENABLE_ONNX "
        "(falling back to the native template rig)."));
}

#else  // ENABLE_ONNX

RigNetPredictor::Result RigNetPredictor::predict(
        const float* positions, int vertexCount,
        const uint32_t* indices, int indexCount,
        const QString& modelPath, const Options& opts)
{
    if (!positions || vertexCount < 4)
        return failResult(QStringLiteral("RigNet: mesh has too few vertices."));
    if (!QFileInfo::exists(modelPath))
        return failResult(QStringLiteral(
            "RigNet model not available at '%1' — connect to the internet to "
            "download it on first use.").arg(modelPath));

    const int up = std::clamp(opts.upAxis, 0, 2);

    // --- Normalise vertices into a centred unit box (+Y up for the model) ---
    // RigNet expects geometry roughly normalised; we record the centre + scale
    // to de-normalise the predicted joints back to mesh-local space.
    std::array<double,3> mn = { 1e30, 1e30, 1e30}, mx = {-1e30,-1e30,-1e30};
    for (int i = 0; i < vertexCount; ++i)
        for (int a = 0; a < 3; ++a) {
            const double v = positions[3*i + a];
            mn[a] = std::min(mn[a], v); mx[a] = std::max(mx[a], v);
        }
    const std::array<double,3> centre = { 0.5*(mn[0]+mx[0]), 0.5*(mn[1]+mx[1]), 0.5*(mn[2]+mx[2]) };
    double scale = 0.0;
    for (int a = 0; a < 3; ++a) scale = std::max(scale, mx[a]-mn[a]);
    if (scale < 1e-9) return failResult(QStringLiteral("RigNet: degenerate mesh bounds."));
    const double inv = 1.0 / scale;

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

    // --- Edge list from triangle indices (undirected, both directions) ------
    // RigNet's graph network consumes mesh edges. We pass them as an int64
    // [2, E] tensor named "edges" when the model declares a second input.
    std::vector<int64_t> edges;
    if (indices && indexCount >= 3) {
        edges.reserve(static_cast<size_t>(indexCount) * 2);
        auto addEdge = [&](uint32_t a, uint32_t b) {
            if (a < static_cast<uint32_t>(vertexCount) && b < static_cast<uint32_t>(vertexCount)) {
                edges.push_back(a); edges.push_back(b);
                edges.push_back(b); edges.push_back(a);
            }
        };
        for (int t = 0; t + 2 < indexCount; t += 3) {
            addEdge(indices[t], indices[t+1]);
            addEdge(indices[t+1], indices[t+2]);
            addEdge(indices[t+2], indices[t]);
        }
    }

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_rignet");
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef __APPLE__
        try {
            std::unordered_map<std::string, std::string> coremlOpts;
            so.AppendExecutionProvider("CoreML", coremlOpts);
        } catch (const Ort::Exception&) {}
#endif
#ifdef _WIN32
        std::wstring wpath = modelPath.toStdWString();
        Ort::Session session(env, wpath.c_str(), so);
#else
        const std::string p = modelPath.toStdString();
        Ort::Session session(env, p.c_str(), so);
#endif
        Ort::AllocatorWithDefaultOptions alloc;
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        // Build inputs by the names the model declares (discover at runtime so
        // we don't hard-code the export's tensor naming).
        const size_t nIn = session.GetInputCount();
        std::vector<Ort::AllocatedStringPtr> inHolders;
        std::vector<const char*> inNames;
        std::vector<Ort::Value> inVals;

        const int64_t vshape[3] = {1, vertexCount, 3};
        const int64_t eshape[2] = {2, static_cast<int64_t>(edges.size() / 2)};

        for (size_t i = 0; i < nIn; ++i) {
            auto nameHolder = session.GetInputNameAllocated(i, alloc);
            const std::string name = nameHolder.get();
            inHolders.push_back(std::move(nameHolder));
            inNames.push_back(inHolders.back().get());
            if (name.find("edge") != std::string::npos ||
                name.find("adj")  != std::string::npos) {
                if (edges.empty())
                    return failResult(QStringLiteral(
                        "RigNet: model needs an edge graph but the mesh has no faces."));
                inVals.push_back(Ort::Value::CreateTensor<int64_t>(
                    mem, edges.data(), edges.size(), eshape, 2));
            } else {
                inVals.push_back(Ort::Value::CreateTensor<float>(
                    mem, nverts.data(), nverts.size(), vshape, 3));
            }
        }
        if (inVals.empty())
            return failResult(QStringLiteral("RigNet: model declares no inputs."));

        // Output names.
        const size_t nOut = session.GetOutputCount();
        std::vector<Ort::AllocatedStringPtr> outHolders;
        std::vector<const char*> outNames;
        for (size_t i = 0; i < nOut; ++i) {
            auto h = session.GetOutputNameAllocated(i, alloc);
            outHolders.push_back(std::move(h));
            outNames.push_back(outHolders.back().get());
        }

        std::vector<Ort::Value> outs;
        try {
            outs = session.Run(Ort::RunOptions{nullptr},
                               inNames.data(), inVals.data(), inVals.size(),
                               outNames.data(), outNames.size());
        } catch (const Ort::Exception& e) {
            return failResult(QStringLiteral("RigNet inference failed: %1")
                .arg(QString::fromUtf8(e.what())));
        }

        // Locate the joint-position output (a float tensor whose last dim is 3)
        // and, optionally, a parent/connectivity output (int tensor).
        const float* jointData = nullptr; int jointCount = 0;
        const int64_t* parentData = nullptr; int parentCount = 0;
        for (size_t i = 0; i < outs.size(); ++i) {
            if (!outs[i].IsTensor()) continue;
            auto info = outs[i].GetTensorTypeAndShapeInfo();
            auto shape = info.GetShape();
            const auto etype = info.GetElementType();
            const int64_t total = info.GetElementCount();
            if (etype == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT &&
                !shape.empty() && shape.back() == 3 && !jointData) {
                jointData = outs[i].GetTensorData<float>();
                jointCount = static_cast<int>(total / 3);
            } else if ((etype == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 ||
                        etype == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32) && !parentData) {
                // Accept int64 parents (int32 handled below by copy).
                if (etype == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
                    parentData = outs[i].GetTensorData<int64_t>();
                    parentCount = static_cast<int>(total);
                }
            }
        }
        if (!jointData || jointCount <= 0)
            return failResult(QStringLiteral(
                "RigNet: model produced no joint-position output."));

        // De-normalise joints back to mesh-local space.
        Result r;
        r.joints.reserve(jointCount);
        for (int j = 0; j < jointCount; ++j) {
            std::array<double,3> p = { jointData[3*j+0], jointData[3*j+1], jointData[3*j+2] };
            p = fromModelUp(p, up);
            p = { p[0]*scale + centre[0], p[1]*scale + centre[1], p[2]*scale + centre[2] };
            Joint jt;
            int parent = -1;
            if (parentData && j < parentCount) {
                parent = static_cast<int>(parentData[j]);
                if (parent < -1 || parent >= jointCount || parent == j) parent = -1;
            }
            jt.parent = parent;
            jt.pos = p;
            jt.name = jointName(j, parent);
            r.joints.push_back(std::move(jt));
        }

        // If the model gave no connectivity, build a connected chain via a
        // nearest-neighbour minimum spanning tree rooted at the lowest joint
        // (a sane default that keeps the skeleton a single tree).
        bool anyParent = false;
        for (const auto& j : r.joints) if (j.parent >= 0) { anyParent = true; break; }
        if (!anyParent && r.joints.size() > 1) {
            const int n = static_cast<int>(r.joints.size());
            int root = 0;
            for (int i = 1; i < n; ++i)
                if (r.joints[i].pos[up] < r.joints[root].pos[up]) root = i;
            std::vector<bool> inTree(n, false);
            inTree[root] = true; r.joints[root].parent = -1;
            for (int added = 1; added < n; ++added) {
                int bestChild = -1, bestParent = -1;
                double bestD = std::numeric_limits<double>::infinity();
                for (int c = 0; c < n; ++c) {
                    if (inTree[c]) continue;
                    for (int pp = 0; pp < n; ++pp) {
                        if (!inTree[pp]) continue;
                        double d = 0;
                        for (int a = 0; a < 3; ++a) {
                            const double dd = r.joints[c].pos[a] - r.joints[pp].pos[a];
                            d += dd*dd;
                        }
                        if (d < bestD) { bestD = d; bestChild = c; bestParent = pp; }
                    }
                }
                if (bestChild < 0) break;
                inTree[bestChild] = true;
                r.joints[bestChild].parent = bestParent;
                r.joints[bestChild].name = jointName(bestChild, bestParent);
            }
        }

        // Ensure parent-before-child ordering (Ogre bone creation needs a valid
        // topo order). Stable reorder: roots first, then BFS.
        {
            const int n = static_cast<int>(r.joints.size());
            std::vector<std::vector<int>> kids(n);
            std::vector<int> roots;
            for (int i = 0; i < n; ++i) {
                if (r.joints[i].parent < 0) roots.push_back(i);
                else kids[r.joints[i].parent].push_back(i);
            }
            std::vector<int> order; order.reserve(n);
            std::vector<int> stack(roots.rbegin(), roots.rend());
            std::vector<bool> seen(n, false);
            while (!order.empty() || !stack.empty()) {
                if (stack.empty()) break;
                int cur = stack.back(); stack.pop_back();
                if (seen[cur]) continue;
                seen[cur] = true; order.push_back(cur);
                for (auto it = kids[cur].rbegin(); it != kids[cur].rend(); ++it)
                    stack.push_back(*it);
            }
            // Any unseen (cycle/garbage) → append as roots.
            for (int i = 0; i < n; ++i) if (!seen[i]) order.push_back(i);
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
        return failResult(QStringLiteral("RigNet ONNX error: %1")
            .arg(QString::fromUtf8(e.what())));
    } catch (const std::exception& e) {
        return failResult(QStringLiteral("RigNet error: %1")
            .arg(QString::fromUtf8(e.what())));
    }
}

#endif  // ENABLE_ONNX
