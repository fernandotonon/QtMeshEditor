#pragma once

#include "HDR/HdrTonemap.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <qqml.h>

class OgreWidget;

/// QML bridge for global HDR / IBL / tonemap controls (Slice E, #471).
class HdrEnvironmentController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentEnvironment READ currentEnvironment NOTIFY environmentChanged)
    Q_PROPERTY(QString currentEnvironmentLabel READ currentEnvironmentLabel NOTIFY environmentChanged)
    Q_PROPERTY(QStringList bundledEnvironments READ bundledEnvironments NOTIFY environmentChoicesChanged)
    Q_PROPERTY(QStringList environmentChoices READ environmentChoices NOTIFY environmentChoicesChanged)
    Q_PROPERTY(int currentChoiceIndex READ currentChoiceIndex NOTIFY environmentChanged)
    Q_PROPERTY(bool hasEnvironment READ hasEnvironment NOTIFY environmentChanged)
    Q_PROPERTY(bool iblReady READ iblReady NOTIFY iblReadyChanged)
    Q_PROPERTY(int tonemapOperator READ tonemapOperator WRITE setTonemapOperator NOTIFY tonemapChanged)
    Q_PROPERTY(float exposureEv READ exposureEv WRITE setExposureEv NOTIFY tonemapChanged)
    Q_PROPERTY(float whitePoint READ whitePoint WRITE setWhitePoint NOTIFY tonemapChanged)
    Q_PROPERTY(bool defaultSkyBoxVisible READ defaultSkyBoxVisible WRITE setDefaultSkyBoxVisible
                   NOTIFY skyboxChanged)
    Q_PROPERTY(float backgroundBlur READ backgroundBlur WRITE setBackgroundBlur NOTIFY backgroundBlurChanged)

public:
    static HdrEnvironmentController* instance();
    static HdrEnvironmentController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    QString currentEnvironment() const;
    QStringList bundledEnvironments() const { return m_bundledEnvironments; }
    QStringList environmentChoices() const { return m_environmentChoices; }
    int currentChoiceIndex() const;
    bool hasEnvironment() const;
    bool iblReady() const;

    int tonemapOperator() const;
    void setTonemapOperator(int op);
    float exposureEv() const;
    void setExposureEv(float value);
    float whitePoint() const;
    void setWhitePoint(float value);
    bool defaultSkyBoxVisible() const;
    void setDefaultSkyBoxVisible(bool visible);
    float backgroundBlur() const;
    void setBackgroundBlur(float blur);

    Q_INVOKABLE bool loadEnvironment(const QString& pathOrBundledName);
    Q_INVOKABLE bool loadEnvironmentChoice(int index);
    Q_INVOKABLE void browseForEnvironment();
    Q_INVOKABLE QString browseEnvironment();
    Q_INVOKABLE void resetTonemap();
    Q_INVOKABLE void setActiveWidget(OgreWidget* widget);
    Q_INVOKABLE QString tonemapOperatorName(int op) const;
    QString currentEnvironmentLabel() const;
    QString browseStartDirectory() const;

    /// Called from MainWindow after the native file dialog returns.
    void completeBrowseFromDialog(const QString& path);

public slots:
    void refreshBundledList();

signals:
    void environmentChanged();
    void environmentChoicesChanged();
    void iblReadyChanged();
    void tonemapChanged();
    void skyboxChanged();
    void backgroundBlurChanged();
    /// Emitted when the user clicks Browse — MainWindow opens QFileDialog with a QWidget parent.
    void browseRequested();

private:
    explicit HdrEnvironmentController(QObject* parent = nullptr);

    OgreWidget* activeWidget() const;
    void connectManagerSignals();
    void loadRecentPaths();
    void rememberRecentPath(const QString& resolvedPath);
    void rebuildEnvironmentChoices();

    static HdrEnvironmentController* s_instance;
    QStringList m_bundledEnvironments;
    QStringList m_environmentChoices;
    QStringList m_choiceLoadKeys;
    QStringList m_recentEnvironments;
    OgreWidget* m_activeWidget = nullptr;
};
