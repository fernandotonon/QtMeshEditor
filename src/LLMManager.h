#ifndef LLMMANAGER_H
#define LLMMANAGER_H

#include <QObject>
#include <QThread>
#include <QSettings>
#include <QStringList>
#include <QVariantMap>
#include <QDir>
#include <QQmlEngine>
#include <QJSEngine>
#include "LLMWorker.h"

struct ModelInfo {
    QString name;
    QString fileName;
    QString url;
    QString description;
    qint64 size;
    bool isDownloaded;

    QVariantMap toVariantMap() const {
        return {
            {"name", name},
            {"fileName", fileName},
            {"url", url},
            {"description", description},
            {"size", size},
            {"isDownloaded", isDownloaded}
        };
    }
};

class LLMManager : public QObject
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
    Q_PROPERTY(int contextSize READ contextSize WRITE setContextSize NOTIFY settingsChanged)
    Q_PROPERTY(int maxTokens READ maxTokens WRITE setMaxTokens NOTIFY settingsChanged)
    Q_PROPERTY(float temperature READ temperature WRITE setTemperature NOTIFY settingsChanged)
    Q_PROPERTY(int gpuLayers READ gpuLayers WRITE setGpuLayers NOTIFY settingsChanged)
    Q_PROPERTY(bool autoLoadModel READ autoLoadModel WRITE setAutoLoadModel NOTIFY autoLoadModelChanged)
    Q_PROPERTY(QString lastModelName READ lastModelName NOTIFY lastModelNameChanged)

public:
    static LLMManager* instance();
    static LLMManager* qmlInstance(QQmlEngine *engine, QJSEngine *scriptEngine);

    // Model management
    bool isModelLoaded() const;
    bool isLoading() const { return m_isLoading; }
    QString currentModelName() const { return m_currentModelName; }
    QStringList availableModels() const;
    QList<ModelInfo> getRecommendedModels() const;

    // Settings
    QString modelsDirectory() const { return m_modelsDirectory; }
    void setModelsDirectory(const QString &dir);
    LLMSettings getSettings() const;
    void setSettings(const LLMSettings &settings);

    // Settings accessors for QML
    int contextSize() const { return m_settings.contextSize; }
    void setContextSize(int value);
    int maxTokens() const { return m_settings.maxTokens; }
    void setMaxTokens(int value);
    float temperature() const { return m_settings.temperature; }
    void setTemperature(float value);
    int gpuLayers() const { return m_settings.gpuLayers; }
    void setGpuLayers(int value);
    bool autoLoadModel() const { return m_autoLoadModel; }
    void setAutoLoadModel(bool value);
    QString lastModelName() const { return m_lastModelName; }

    // Generation state
    bool isGenerating() const;

    // System prompt for Ogre3D materials
    static QString getOgre3DSystemPrompt();

public slots:
    // Model operations
    Q_INVOKABLE void loadModel(const QString &modelName);
    Q_INVOKABLE void unloadModel();
    Q_INVOKABLE void scanForModels();
    Q_INVOKABLE void tryAutoLoadModel();

    // Generation
    Q_INVOKABLE void generateMaterial(const QString &prompt, const QString &currentMaterial = QString());
    Q_INVOKABLE void stopGeneration();

    // Settings
    Q_INVOKABLE void saveSettings();
    Q_INVOKABLE void loadSettings();

    // Utility
    Q_INVOKABLE QString getModelFilePath(const QString &modelName) const;
    Q_INVOKABLE bool modelFileExists(const QString &modelName) const;
    Q_INVOKABLE QVariantList getAvailableModelsInfo() const;
    Q_INVOKABLE QVariantList getRecommendedModelsInfo() const;

public:
    // Script validation (internal use - cannot be slots due to reference parameter)
    bool validateMaterialScript(const QString &script, QString &errorMessage) const;
    QString cleanupGeneratedScript(const QString &script) const;

signals:
    void modelLoadedChanged();
    void currentModelNameChanged();
    void isGeneratingChanged();
    void isLoadingChanged();
    void availableModelsChanged();
    void modelsDirectoryChanged();
    void settingsChanged();
    void autoLoadModelChanged();
    void lastModelNameChanged();

    void modelLoadStarted(const QString &modelName);
    void modelLoadProgress(float progress);
    void modelLoadCompleted(const QString &modelName);
    void modelLoadError(const QString &error);
    void modelUnloaded();

    void generationStarted();
    void generationProgress(const QString &partialText, float progress);
    void generationCompleted(const QString &generatedText);
    void generationError(const QString &error);
    void generationStopped();

private:
    explicit LLMManager(QObject *parent = nullptr);
    ~LLMManager();

    void initializeWorkerThread();
    void shutdownWorkerThread();
    QString getDefaultModelsDirectory() const;
    void populateRecommendedModels();

private slots:
    void onWorkerModelLoaded(const QString &modelPath);
    void onWorkerModelLoadError(const QString &error);
    void onWorkerModelUnloaded();
    void onWorkerGenerationStarted();
    void onWorkerGenerationProgress(const QString &partialText, float progress);
    void onWorkerGenerationCompleted(const QString &fullText);
    void onWorkerGenerationError(const QString &error);
    void onWorkerGenerationStopped();

private:
    static LLMManager* s_instance;

    QThread *m_workerThread = nullptr;
    LLMWorker *m_worker = nullptr;

    QString m_modelsDirectory;
    QString m_currentModelName;
    QString m_lastModelName;
    QStringList m_availableModels;
    QList<ModelInfo> m_recommendedModels;
    LLMSettings m_settings;
    bool m_isLoading = false;
    bool m_autoLoadModel = false;

    // Retry logic for invalid scripts
    QString m_pendingPrompt;
    QString m_pendingCurrentMaterial;
    int m_retryCount = 0;
    static const int MAX_RETRIES = 2;
};

#endif // LLMMANAGER_H
