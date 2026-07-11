#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QStringList>

class LightGroupController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool canGroupSelection READ canGroupSelection NOTIFY selectionChanged)
    Q_PROPERTY(bool hasLightGroupSelection READ hasLightGroupSelection NOTIFY selectionChanged)
    Q_PROPERTY(QStringList lightGroupNames READ lightGroupNames NOTIFY groupsChanged)
    Q_PROPERTY(QString selectedGroupName READ selectedGroupName NOTIFY selectionChanged)

public:
    static LightGroupController* instance();
    static LightGroupController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    bool canGroupSelection() const;
    bool hasLightGroupSelection() const;
    QStringList lightGroupNames() const;
    QString selectedGroupName() const;

    Q_INVOKABLE void createGroupFromSelection(const QString& groupName);
    Q_INVOKABLE void dissolveSelectedGroup();
    Q_INVOKABLE void setSelectedGroupEnabled(bool enabled);

signals:
    void selectionChanged();
    void groupsChanged();

private:
    explicit LightGroupController(QObject* parent = nullptr);

    QStringList selectedLightNames() const;
    QString selectedLightGroupNodeName() const;

    static LightGroupController* s_singleton;
};
