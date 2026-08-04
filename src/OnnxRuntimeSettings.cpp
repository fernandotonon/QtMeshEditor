#include "OnnxRuntimeSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QSettings>
#include <cstdio>
#include <thread>

#ifdef ENABLE_ONNX
#include <unordered_map>
#endif

namespace {
constexpr const char* kPreferGpuKey = "ai/onnxPreferGpu";

#ifdef QTMESH_ONNX_GPU_BUILD
void addCudaDepsTree(const QString& root, QStringList& paths)
{
    const QString cudnn = QDir::cleanPath(root + QStringLiteral("/nvidia/cudnn/lib"));
    const QString cublas = QDir::cleanPath(root + QStringLiteral("/nvidia/cublas/lib"));
    if (QFileInfo::exists(cudnn + QStringLiteral("/libcudnn.so.9"))
        && !paths.contains(cudnn))
        paths << cudnn;
    if (QFileInfo(cublas).isDir() && !paths.contains(cublas))
        paths << cublas;
}

QStringList cudnnSearchPaths()
{
    QStringList paths;
    auto addIfCudnn = [&](const QString& dir) {
        const QString d = QDir::cleanPath(dir);
        if (QFileInfo::exists(d + QStringLiteral("/libcudnn.so.9"))
            && !paths.contains(d))
            paths << d;
    };

    const QByteArray env = qgetenv("QTMESH_CUDNN_LIB_DIR");
    for (const QByteArray& part : env.split(':')) {
        if (!part.isEmpty())
            addIfCudnn(QString::fromUtf8(part));
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    addCudaDepsTree(appDir + QStringLiteral("/cuda-deps"), paths);
    addCudaDepsTree(appDir + QStringLiteral("/../cuda-deps"), paths);

    QDir repo(appDir);
    if (repo.cdUp() && repo.cdUp()) {
        const QString root = repo.absolutePath();
        addCudaDepsTree(root + QStringLiteral("/.cache/cuda-deps"), paths);
        addCudaDepsTree(root + QStringLiteral("/cuda-deps"), paths);
    }

    addIfCudnn(QStringLiteral("/usr/lib/x86_64-linux-gnu"));
    addIfCudnn(QStringLiteral("/usr/local/cuda/lib64"));
    return paths;
}

void prependLdLibraryPath(const QStringList& dirs)
{
    if (dirs.isEmpty())
        return;
    QStringList merged = dirs;
    const QByteArray old = qgetenv("LD_LIBRARY_PATH");
    if (!old.isEmpty())
        merged << QString::fromUtf8(old).split(':',
            Qt::SkipEmptyParts);
    qputenv("LD_LIBRARY_PATH", merged.join(':').toUtf8());
    if (qEnvironmentVariableIsSet("QTMESH_ONNX_DEBUG")) {
        fprintf(stderr, "[onnx] LD_LIBRARY_PATH=%s\n",
                qgetenv("LD_LIBRARY_PATH").constData());
    }
}

bool cudaProviderLibraryLoads()
{
    // Do NOT dlopen libonnxruntime_providers_cuda.so here — it must be loaded
    // by libonnxruntime.so (standalone load fails: undefined Provider_GetHost).
    for (const QString& dir : cudnnSearchPaths()) {
        const QString cudnn = dir + QStringLiteral("/libcudnn.so.9");
        if (!QFileInfo::exists(cudnn))
            continue;
        QLibrary lib(cudnn);
        if (lib.load()) {
            lib.unload();
            return true;
        }
        if (qEnvironmentVariableIsSet("QTMESH_ONNX_DEBUG")) {
            fprintf(stderr, "[onnx] libcudnn load failed (%s): %s\n",
                    dir.toUtf8().constData(),
                    lib.errorString().toUtf8().constData());
        }
    }
    return false;
}
#endif

} // namespace

void OnnxRuntimeSettings::prepareRuntimeEnvironment()
{
#ifdef QTMESH_ONNX_GPU_BUILD
    static bool done = false;
    if (done)
        return;
    done = true;
    prependLdLibraryPath(cudnnSearchPaths());
#endif
}

OnnxRuntimeSettings::OnnxRuntimeSettings(QObject* parent)
    : QObject(parent)
{
    loadSettings();
    prepareRuntimeEnvironment();
    refreshGpuProviderStatus();
}

OnnxRuntimeSettings* OnnxRuntimeSettings::instance()
{
    static OnnxRuntimeSettings inst;
    return &inst;
}

OnnxRuntimeSettings* OnnxRuntimeSettings::qmlInstance(QQmlEngine* engine,
                                                      QJSEngine* scriptEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(scriptEngine)
    return instance();
}

bool OnnxRuntimeSettings::defaultPreferGpu()
{
#if defined(__APPLE__) || defined(QTMESH_ONNX_GPU_BUILD)
    return true;
#else
    return false;
#endif
}

bool OnnxRuntimeSettings::onnxAvailable() const
{
#ifdef ENABLE_ONNX
    return true;
#else
    return false;
#endif
}

bool OnnxRuntimeSettings::preferGpu() const
{
    if (qEnvironmentVariableIsSet("QTMESH_ONNX_PREFER_GPU"))
        return qEnvironmentVariableIntValue("QTMESH_ONNX_PREFER_GPU") != 0;
    return m_preferGpu;
}

void OnnxRuntimeSettings::setPreferGpu(bool value)
{
    if (m_preferGpu == value)
        return;
    m_preferGpu = value;
    saveSettings();
    updateGpuProviderNoteLocked();
    emit settingsChanged();
}

bool OnnxRuntimeSettings::gpuProviderBundled() const
{
    return m_gpuProviderBundled;
}

bool OnnxRuntimeSettings::gpuProviderReady() const
{
    return m_gpuProviderReady;
}

QString OnnxRuntimeSettings::gpuProviderNote() const
{
    return m_gpuProviderNote;
}

void OnnxRuntimeSettings::refreshGpuProviderStatus()
{
#ifdef QTMESH_ONNX_GPU_BUILD
    prepareRuntimeEnvironment();
    const QString cudaSo =
        QCoreApplication::applicationDirPath()
        + QStringLiteral("/libonnxruntime_providers_cuda.so");
    m_gpuProviderBundled = QFileInfo::exists(cudaSo);
    m_gpuProviderReady   = m_gpuProviderBundled && cudaProviderLibraryLoads();
#else
    m_gpuProviderBundled = false;
    m_gpuProviderReady   = false;
#endif
    updateGpuProviderNoteLocked();
    emit gpuStatusChanged();
    emit settingsChanged();
}

void OnnxRuntimeSettings::updateGpuProviderNoteLocked()
{
#ifndef ENABLE_ONNX
    m_gpuProviderNote = tr("ONNX is not enabled in this build.");
#elif defined(__APPLE__)
    if (!preferGpu()) {
        m_gpuProviderNote = tr("CPU only.");
        return;
    }
    m_gpuProviderNote = tr("Uses CoreML (GPU/ANE) when available; falls back to CPU.");
#elif defined(_WIN32) && !defined(__MINGW32__)
    if (!preferGpu()) {
        m_gpuProviderNote = tr("CPU only.");
        return;
    }
    m_gpuProviderNote =
        tr("Attempts DirectML when available; rebuild with -DQTMESH_ONNX_GPU=ON for the GPU package.");
#elif defined(QTMESH_ONNX_GPU_BUILD)
    if (!m_gpuProviderBundled) {
        m_gpuProviderNote =
            tr("GPU ONNX Runtime was enabled at build time but libonnxruntime_providers_cuda.so "
               "is missing next to the executable — rebuild QtMeshEditor.");
        return;
    }
    if (!m_gpuProviderReady) {
        m_gpuProviderNote =
            tr("CUDA provider found but cuDNN 9 is missing — run "
               "scripts/install-onnx-gpu-deps.sh (installs to .cache/cuda-deps). "
               "Until then UniRig runs on CPU.");
        return;
    }
    m_gpuProviderNote = preferGpu()
        ? tr("CUDA execution provider ready (NVIDIA GPU).")
        : tr("CUDA available but disabled — enable “Prefer GPU” above.");
#else
    if (!preferGpu()) {
        m_gpuProviderNote = tr("CPU only.");
        return;
    }
    m_gpuProviderNote =
        tr("This build bundles CPU-only ONNX Runtime. Rebuild with "
           "-DQTMESH_ONNX_GPU=ON (auto-detected when nvidia-smi is present) to use your NVIDIA GPU.");
#endif
}

void OnnxRuntimeSettings::loadSettings()
{
    QSettings settings;
    m_preferGpu = settings.value(kPreferGpuKey, defaultPreferGpu()).toBool();
    updateGpuProviderNoteLocked();
}

void OnnxRuntimeSettings::saveSettings()
{
    QSettings settings;
    settings.setValue(kPreferGpuKey, m_preferGpu);
}

#ifdef ENABLE_ONNX
void OnnxRuntimeSettings::configureSessionOptions(Ort::SessionOptions& so)
{
    configureSessionOptions(so, SessionConfig{});
}

void OnnxRuntimeSettings::configureSessionOptions(Ort::SessionOptions& so,
                                                  const SessionConfig& cfg)
{
    so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    if (cfg.reserveUiThreadCore) {
        const unsigned hc = std::thread::hardware_concurrency();
        so.SetIntraOpNumThreads(hc > 1 ? static_cast<int>(hc - 1) : 1);
    }
    if (!cfg.allowSpinning)
        so.AddConfigEntry("session.intra_op.allow_spinning", "0");
    if (cfg.appendGpu && instance()->preferGpu()) {
        if (!tryAppendGpuExecutionProvider(so, cfg.coreMlStyle)
            && qEnvironmentVariableIsSet("QTMESH_ONNX_DEBUG")) {
            fprintf(stderr,
                    "[onnx] preferGpu=1 but no GPU execution provider was registered "
                    "(see AI Settings → ONNX note)\n");
        }
    }
}

bool OnnxRuntimeSettings::tryAppendGpuExecutionProvider(Ort::SessionOptions& so,
                                                        CoreMlStyle style)
{
    prepareRuntimeEnvironment();
#ifdef __APPLE__
    try {
        std::unordered_map<std::string, std::string> opts;
        if (style == CoreMlStyle::MlProgram) {
            opts["ModelFormat"]    = "MLProgram";
            opts["MLComputeUnits"] = "ALL";
        }
        so.AppendExecutionProvider("CoreML", opts);
        return true;
    } catch (const Ort::Exception& e) {
        if (qEnvironmentVariableIsSet("QTMESH_ONNX_DEBUG"))
            fprintf(stderr, "[onnx] CoreML EP failed: %s\n", e.what());
    }
#elif defined(_WIN32) && !defined(__MINGW32__)
    try {
        so.AppendExecutionProvider("DML", {});
        return true;
    } catch (const Ort::Exception& e) {
        if (qEnvironmentVariableIsSet("QTMESH_ONNX_DEBUG"))
            fprintf(stderr, "[onnx] DirectML EP failed: %s\n", e.what());
    }
#elif defined(__linux__)
    try {
        OrtCUDAProviderOptions cuda{};
        cuda.device_id = 0;
        so.AppendExecutionProvider_CUDA(cuda);
        if (qEnvironmentVariableIsSet("QTMESH_ONNX_DEBUG"))
            fprintf(stderr, "[onnx] CUDA execution provider registered\n");
        return true;
    } catch (const Ort::Exception& e) {
        if (qEnvironmentVariableIsSet("QTMESH_ONNX_DEBUG"))
            fprintf(stderr, "[onnx] CUDA EP failed: %s\n", e.what());
    }
#endif
    return false;
}
#endif // ENABLE_ONNX
