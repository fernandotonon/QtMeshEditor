#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QStringList>

class SceneLightingController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList rigIds READ rigIds CONSTANT)
    Q_PROPERTY(QStringList rigNames READ rigNames CONSTANT)
    Q_PROPERTY(int selectedRigIndex READ selectedRigIndex WRITE setSelectedRigIndex NOTIFY selectedRigIndexChanged)
    Q_PROPERTY(bool replaceExistingLights READ replaceExistingLights WRITE setReplaceExistingLights
                   NOTIFY replaceExistingLightsChanged)

public:
    static SceneLightingController* instance();
    static SceneLightingController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    QStringList rigIds() const;
    QStringList rigNames() const;

    int selectedRigIndex() const;
    void setSelectedRigIndex(int index);

    bool replaceExistingLights() const;
    void setReplaceExistingLights(bool replace);

    Q_INVOKABLE void applySelectedRig();
    Q_INVOKABLE void applyRig(const QString& rigId, bool replaceExisting);

signals:
    void selectedRigIndexChanged();
    void replaceExistingLightsChanged();

private:
    explicit SceneLightingController(QObject* parent = nullptr);

    void maybeLoadSuggestedHdri(const QString& hdriName);
    bool confirmReplaceExisting(QWidget* parent) const;

    static SceneLightingController* s_singleton;

    int m_selectedRigIndex = 0;
    bool m_replaceExistingLights = true;
};
