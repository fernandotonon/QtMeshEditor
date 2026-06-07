#ifndef SDMANAGER_H
#define SDMANAGER_H

#include <QObject>
#include <QThread>
#include <QSettings>
#include <QStringList>
#include <QVariantMap>
#include <QDir>
#include <QQmlEngine>
#include <QJSEngine>
#include "SDWorker.h"

struct SDModelInfo {
    QString name;
    QString fileName;
    QString url;
    QString description;
    qint64 size;
    bool isDownloaded;
    // "base" = a full SD model loadable as the generation context.
    // "controlnet" = a ControlNet conditioning model (issue #403),
    // NOT loadable as a base model — used only via the mesh-texture
    // dialog's ControlNet field. Defaults to "base" so existing
    // entries are unaffected.
    QString kind = QStringLiteral("base");

    QVariantMap toVariantMap() const {
        return {
            {"name", name},
            {"fileName", fileName},
            {"url", url},
            {"description", description},
            {"size", size},
            {"isDownloaded", isDownloaded},
            {"kind", kind}
        };
    }
};

class SDManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool modelLoaded READ isModelLoaded NOTIFY modelLoadedChanged)
    Q_PROPERTY(QString currentModelName READ currentModelName NOTIFY currentModelNameChanged)
    Q_PROPERTY(bool isGenerating READ isGenerating NOTIFY isGeneratingChanged)
    Q_PROPERTY(bool isLoading READ isLoading NOTIFY isLoadingChanged)
    Q_PROPERTY(QStringList availableModels READ availableModels NOTIFY availableModelsChanged)
    Q_PROPERTY(QString modelsDirectory READ modelsDirectory WRITE setModelsDirectory NOTIFY modelsDirectoryChanged)
    Q_PROPERTY(QVariantList recommendedModels READ getRecommendedModelsInfo NOTIFY availableModelsChanged)

    // Settings properties for QML binding
    Q_PROPERTY(int imageWidth READ imageWidth WRITE setImageWidth NOTIFY settingsChanged)
    Q_PROPERTY(int imageHeight READ imageHeight WRITE setImageHeight NOTIFY settingsChanged)
    Q_PROPERTY(int steps READ steps WRITE setSteps NOTIFY settingsChanged)
    Q_PROPERTY(float cfgScale READ cfgScale WRITE setCfgScale NOTIFY settingsChanged)
    Q_PROPERTY(QString negativePrompt READ negativePrompt WRITE setNegativePrompt NOTIFY settingsChanged)
    Q_PROPERTY(int generationStep READ generationStep NOTIFY generationProgressChanged)
    Q_PROPERTY(int generationTotalSteps READ generationTotalSteps NOTIFY generationProgressChanged)
    Q_PROPERTY(bool autoLoadModel READ autoLoadModel WRITE setAutoLoadModel NOTIFY autoLoadModelChanged)
    Q_PROPERTY(QString lastModelName READ lastModelName NOTIFY lastModelNameChanged)

public:
    static SDManager* instance();
    static SDManager* qmlInstance(QQmlEngine *engine, QJSEngine *scriptEngine);

    // Model management
    bool isModelLoaded() const;
    bool isLoading() const { return m_isLoading; }
    QString currentModelName() const { return m_currentModelName; }
    QStringList availableModels() const;

    // Settings
    QString modelsDirectory() const { return m_modelsDirectory; }
    void setModelsDirectory(const QString &dir);
    SDSettings getSettings() const;
    void setSettings(const SDSettings &settings);

    // Settings accessors for QML
    int imageWidth() const { return m_settings.width; }
    void setImageWidth(int value);
    int imageHeight() const { return m_settings.height; }
    void setImageHeight(int value);
    int steps() const { return m_settings.steps; }
    void setSteps(int value);
    float cfgScale() const { return m_settings.cfgScale; }
    void setCfgScale(float value);
    QString negativePrompt() const { return m_settings.negativePrompt; }
    void setNegativePrompt(const QString &value);

    // Prompt enhancement for 3D textures
    static QString enhanceTexturePrompt(const QString &prompt);
    static QString getTextureNegativePrompt();

    // Generation state
    bool isGenerating() const;
    int generationStep() const { return m_generationStep; }
    int generationTotalSteps() const { return m_generationTotalSteps; }

    // Auto-load
    bool autoLoadModel() const { return m_autoLoadModel; }
    void setAutoLoadModel(bool value);
    QString lastModelName() const { return m_lastModelName; }

public slots:
    Q_INVOKABLE void loadModel(const QString &modelName);
    Q_INVOKABLE void unloadModel();
    Q_INVOKABLE void scanForModels();

    Q_INVOKABLE void generateTexture(const QString &prompt, int width = 0, int height = 0, const QString &outputFileName = QString());
    // img2img disabled — crashes on macOS Metal. Edits use txt2img with combined prompt.

    // Issue #403: mesh-aware (depth-conditioned) generation. Same
    // flow as generateTexture but conditions on `controlImage` (a
    // rendered depth map) via a ControlNet model at `controlNetPath`.
    // When controlImage is null or controlNetPath empty, behaves
    // like a plain generateTexture. `controlStrength` is 0..1.
    void generateMeshTexture(const QString &prompt,
                             const QImage &controlImage,
                             const QString &controlNetPath,
                             float controlStrength,
                             const QString &outputFileName = QString());

    Q_INVOKABLE void stopGeneration();
    Q_INVOKABLE void tryAutoLoadModel();

    Q_INVOKABLE void saveSettings();
    Q_INVOKABLE void loadSettings();

    Q_INVOKABLE QString getModelFilePath(const QString &modelName) const;
    Q_INVOKABLE bool modelFileExists(const QString &modelName) const;
    Q_INVOKABLE QVariantList getAvailableModelsInfo() const;
    Q_INVOKABLE QVariantList getRecommendedModelsInfo() const;

signals:
    void modelLoadedChanged();
    void currentModelNameChanged();
    void isGeneratingChanged();
    void isLoadingChanged();
    void availableModelsChanged();
    void modelsDirectoryChanged();
    void settingsChanged();
    void generationProgressChanged();

    void modelLoadStarted(const QString &modelName);
    void modelLoadCompleted(const QString &modelName);
    void modelLoadError(const QString &error);
    void modelUnloaded();

    void generationStarted();
    void generationCompleted(const QString &outputPath);
    void generationError(const QString &error);
    void generationStopped();
    void autoLoadModelChanged();
    void lastModelNameChanged();

private:
    explicit SDManager(QObject *parent = nullptr);
    ~SDManager();

    void initializeWorkerThread();
    void shutdownWorkerThread();
    QString getDefaultModelsDirectory() const;
    void populateRecommendedModels();
    QString generateOutputPath() const;

private slots:
    void onWorkerModelLoaded(const QString &modelPath);
    void onWorkerModelLoadError(const QString &error);
    void onWorkerModelUnloaded();
    void onWorkerGenerationStarted();
    void onWorkerGenerationProgress(int step, int totalSteps);
    void onWorkerGenerationCompleted(const QString &outputPath);
    void onWorkerGenerationError(const QString &error);
    void onWorkerGenerationStopped();

private:
    static SDManager* s_instance;

    QThread *m_workerThread = nullptr;
    SDWorker *m_worker = nullptr;

    QString m_modelsDirectory;
    QString m_currentModelName;
    QStringList m_availableModels;
    QList<SDModelInfo> m_recommendedModels;
    SDSettings m_settings;
    bool m_isLoading = false;
    bool m_autoLoadModel = true;
    QString m_lastModelName;

    int m_generationStep = 0;
    int m_generationTotalSteps = 0;
};

#endif // SDMANAGER_H
