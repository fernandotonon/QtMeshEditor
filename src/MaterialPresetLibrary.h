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

    /// Canonical TUS slot names for PBR templates. Slice E ships the
    /// material structure; slice F will swap in real PBR shading by
    /// reading these names + the "pbr_workflow" user-object binding
    /// without recreating the material.
    static const char* kPbrWorkflowKey;          // "pbr_workflow" (user-binding key)
    static const char* kPbrWorkflowMetallic;     // "metallic_roughness"
    static const char* kPbrWorkflowSpecular;     // "specular_glossiness"
    static const char* kPbrWorkflowUnlit;        // "unlit"

signals:
    void presetApplied(const QString& name);

private:
    MaterialPresetLibrary();
    ~MaterialPresetLibrary() override = default;
    static MaterialPresetLibrary* m_pSingleton;
};

#endif // MATERIAL_PRESET_LIBRARY_H
