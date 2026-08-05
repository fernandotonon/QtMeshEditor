#ifndef ONNXRUNTIMESETTINGS_H
#define ONNXRUNTIMESETTINGS_H

#include <QObject>
#include <QQmlEngine>
#include <QJSEngine>

#ifdef ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

// Central ONNX Runtime session preferences (issue #408 follow-up): thread pool
// sizing, spinning, and optional GPU execution providers. Persisted under
// QSettings ai/onnxPreferGpu and surfaced in AI Settings → Settings tab.
class OnnxRuntimeSettings : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool onnxAvailable READ onnxAvailable CONSTANT)
    Q_PROPERTY(bool preferGpu READ preferGpu WRITE setPreferGpu NOTIFY settingsChanged)
    Q_PROPERTY(bool gpuProviderBundled READ gpuProviderBundled NOTIFY gpuStatusChanged)
    Q_PROPERTY(bool gpuProviderReady READ gpuProviderReady NOTIFY gpuStatusChanged)
    Q_PROPERTY(QString gpuProviderNote READ gpuProviderNote NOTIFY settingsChanged)

public:
    static OnnxRuntimeSettings* instance();
    static OnnxRuntimeSettings* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);

    bool onnxAvailable() const;

    bool preferGpu() const;
    void setPreferGpu(bool value);

    QString gpuProviderNote() const;

    // True when this binary was built against the GPU ONNX Runtime package AND
    // the CUDA provider .so was copied next to the executable.
    bool gpuProviderBundled() const;
    // True when bundled GPU EP exists and runtime deps (cuDNN 9, CUDA 12) load.
    bool gpuProviderReady() const;

    Q_INVOKABLE void refreshGpuProviderStatus();

#ifdef ENABLE_ONNX
    enum class CoreMlStyle { Legacy, MlProgram };

    struct SessionConfig {
        bool reserveUiThreadCore = true;
        bool allowSpinning       = false;
        bool appendGpu           = true;
        CoreMlStyle coreMlStyle  = CoreMlStyle::Legacy;
    };

    // Apply graph optimization, thread count, spinning, and (when preferGpu())
    // the best available GPU EP for this platform/build.
    static void configureSessionOptions(Ort::SessionOptions& so);
    static void configureSessionOptions(Ort::SessionOptions& so,
                                        const SessionConfig& cfg);

    // Best-effort GPU EP append; returns true when one was registered.
    static bool tryAppendGpuExecutionProvider(Ort::SessionOptions& so,
                                              CoreMlStyle style = CoreMlStyle::Legacy);
#endif

    static bool defaultPreferGpu();

    // Must run once before the first Ort::Session (main + CLI entry). On Linux
    // GPU builds this prepends cuDNN/cuBLAS dirs to LD_LIBRARY_PATH.
    static void prepareRuntimeEnvironment();

    Q_INVOKABLE void loadSettings();
    Q_INVOKABLE void saveSettings();

signals:
    void settingsChanged();
    void gpuStatusChanged();

private:
    explicit OnnxRuntimeSettings(QObject* parent = nullptr);

    void updateGpuProviderNoteLocked();

    bool m_preferGpu = defaultPreferGpu();
    bool m_gpuProviderBundled = false;
    bool m_gpuProviderReady = false;
    QString m_gpuProviderNote;
};

#endif // ONNXRUNTIMESETTINGS_H
