#ifndef EDITORMODECONTROLLER_H
#define EDITORMODECONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QVariantList>

class EditModeController;

class EditorModeController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(int currentMode READ currentMode WRITE setCurrentMode NOTIFY modeChanged)
    Q_PROPERTY(QString modeName READ modeName NOTIFY modeChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(bool editModeAvailable READ editModeAvailable NOTIFY editModeAvailabilityChanged)
    // `availableModes` is `CONSTANT` because the set of editor modes is fixed
    // at compile time. If a future plugin/feature flag ever needs to gate a
    // mode at runtime (e.g. hiding ValidationMode when the scan engine is
    // disabled), drop CONSTANT, add an `availableModesChanged()` signal, and
    // emit it whenever the list changes — otherwise QML bindings on this
    // property will not refresh.
    Q_PROPERTY(QVariantList availableModes READ availableModes CONSTANT)

public:
    enum Mode {
        ObjectMode = 0,
        EditMode = 1,
        AnimationMode = 2,
        MaterialMode = 3,
        ValidationMode = 4
    };
    Q_ENUM(Mode)

    enum InspectorTabId {
        InspectorTab = 0,
        SceneTab = 1,
        ModeToolsTab = 2,
        HistoryTab = 3
    };
    Q_ENUM(InspectorTabId)

    static EditorModeController* instance();
    static EditorModeController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    int currentMode() const { return static_cast<int>(m_currentMode); }
    void setCurrentMode(int mode);

    QString modeName() const;
    QString statusText() const { return m_statusText; }
    bool editModeAvailable() const;
    QVariantList availableModes() const;

    Q_INVOKABLE void requestMode(int mode);
    Q_INVOKABLE void toggleObjectEditMode();
    Q_INVOKABLE QString modeNameFor(int mode) const;
    Q_INVOKABLE QString modeTooltipFor(int mode) const;
    Q_INVOKABLE bool modeHasModeTools(int mode) const;
    Q_INVOKABLE int defaultInspectorTabForMode(int mode) const;
    Q_INVOKABLE bool shouldKeepExplicitInspectorTab(int tab) const;
    Q_INVOKABLE bool modeToolMatches(int sectionMode, bool showAllModeTools) const;
    Q_INVOKABLE bool modeToolMatchesCurrentMode(
        int sectionMode, bool showAllModeTools, int currentMode) const;

signals:
    void modeChanged();
    void statusTextChanged();
    void editModeAvailabilityChanged();

private slots:
    void syncFromEditMode();
    void refreshEditAvailability();

private:
    explicit EditorModeController(QObject* parent = nullptr);
    void setModeInternal(Mode mode, bool driveEditController);
    void setStatusText(const QString& text);
    EditModeController* editController() const;

    static EditorModeController* m_pSingleton;
    Mode m_currentMode = ObjectMode;
    QString m_statusText;
};

#endif // EDITORMODECONTROLLER_H
