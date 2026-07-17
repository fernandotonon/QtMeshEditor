#ifndef AIMODELCATALOG_H
#define AIMODELCATALOG_H

#include <QObject>
#include <QJSEngine>
#include <QQmlEngine>
#include <QVariantList>

class AIModelCatalog : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QVariantList models READ models NOTIFY modelsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString activeModelId READ activeModelId NOTIFY activeModelChanged)
    Q_PROPERTY(QString activeModelName READ activeModelName NOTIFY activeModelChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

public:
    static AIModelCatalog* instance();
    static AIModelCatalog* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);

    QVariantList models() const;
    bool busy() const { return m_busy; }
    QString activeModelId() const { return m_activeModelId; }
    QString activeModelName() const { return m_activeModelName; }
    QString statusMessage() const { return m_statusMessage; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void downloadModel(const QString& id);
    Q_INVOKABLE void downloadAllModels();
    Q_INVOKABLE void deleteModel(const QString& id);
    Q_INVOKABLE void deleteAllModels();
    Q_INVOKABLE QString modelsRootUrl() const;

signals:
    void modelsChanged();
    void busyChanged();
    void activeModelChanged();
    void statusMessageChanged();

private:
    struct FileSpec {
        QString fileName;
        QString path;
        QString url;
        QString label;
    };

    struct ModelSpec {
        QString id;
        QString name;
        QString feature;
        QString description;
        QString size;
        QString buildRequirement;
        bool available = true;
        QList<FileSpec> files;
    };

    explicit AIModelCatalog(QObject* parent = nullptr);

    QList<ModelSpec> specs() const;
    QVariantMap toVariantMap(const ModelSpec& spec) const;
    bool isDownloaded(const ModelSpec& spec) const;
    qint64 installedBytes(const ModelSpec& spec) const;
    QString rootPath() const;
    QString resolveBaseUrl(const char* settingsKey, const char* envKey,
                           const char* fallback) const;
    void setBusy(bool busy);
    void setStatusMessage(const QString& message);
    void startNextQueuedFile();
    void clearActive();
    const ModelSpec* findSpec(const QString& id, QList<ModelSpec>* owner = nullptr) const;

    static AIModelCatalog* s_instance;

    bool m_busy = false;
    QString m_activeModelId;
    QString m_activeModelName;
    QString m_statusMessage;
    QList<FileSpec> m_pendingFiles;
};

#endif // AIMODELCATALOG_H
