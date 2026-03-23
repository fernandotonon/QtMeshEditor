#ifndef MATERIAL_PRESET_LIBRARY_H
#define MATERIAL_PRESET_LIBRARY_H

#include <QObject>
#include <QStringList>
#include <QQmlEngine>

class MaterialPresetLibrary : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QStringList presetNames READ presetNames CONSTANT)

public:
    static MaterialPresetLibrary* instance();
    static MaterialPresetLibrary* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    QStringList presetNames() const;
    Q_INVOKABLE void applyPreset(const QString& name);

signals:
    void presetApplied(const QString& name);

private:
    MaterialPresetLibrary();
    ~MaterialPresetLibrary() override = default;
    static MaterialPresetLibrary* m_pSingleton;
};

#endif // MATERIAL_PRESET_LIBRARY_H
