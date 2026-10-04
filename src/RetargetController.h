/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef RETARGETCONTROLLER_H
#define RETARGETCONTROLLER_H

#include "AnimationRetargeter.h"

#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <OgreVector.h>

#include <map>
#include <string>

namespace Ogre { class Entity; }

/**
 * @brief QML_SINGLETON behind the "Retarget Animation" wizard (#523).
 *
 * Flow: pick a SOURCE entity + clip (or import a file), pick a TARGET
 * entity, auto-map → review/override the bone map → optional side-by-side
 * preview → Apply (one undo step, RetargetAnimationCommand).
 *
 * Entities are held by NAME and re-resolved on every use, so deleting one
 * mid-session never leaves a dangling pointer. The preview writes a
 * temporary clip on the target, plays it alongside the source clip on a
 * timer of its own (independent of the global transport), nudges the
 * target sideways when the two overlap, and restores everything — clip,
 * animation-state flags, node position — when it stops.
 */
class RetargetController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QStringList skeletalEntities READ skeletalEntities NOTIFY entitiesChanged)
    Q_PROPERTY(bool canRetarget READ canRetarget NOTIFY entitiesChanged)
    Q_PROPERTY(QString sourceEntity READ sourceEntity WRITE setSourceEntity NOTIFY selectionChanged)
    Q_PROPERTY(QString targetEntity READ targetEntity WRITE setTargetEntity NOTIFY selectionChanged)
    Q_PROPERTY(QStringList sourceAnimations READ sourceAnimations NOTIFY selectionChanged)
    Q_PROPERTY(QString sourceAnimation READ sourceAnimation WRITE setSourceAnimation NOTIFY selectionChanged)
    Q_PROPERTY(QStringList targetBones READ targetBones NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList mappingRows READ mappingRows NOTIFY mappingChanged)
    Q_PROPERTY(int mappedCount READ mappedCount NOTIFY mappingChanged)
    Q_PROPERTY(QStringList bundledMaps READ bundledMaps CONSTANT)
    Q_PROPERTY(QString translationMode READ translationMode WRITE setTranslationMode NOTIFY optionsChanged)
    Q_PROPERTY(bool alignDirections READ alignDirections WRITE setAlignDirections NOTIFY optionsChanged)
    Q_PROPERTY(QString sourceRest READ sourceRest WRITE setSourceRest NOTIFY optionsChanged)
    Q_PROPERTY(QString newAnimationName READ newAnimationName WRITE setNewAnimationName NOTIFY optionsChanged)
    Q_PROPERTY(bool previewing READ previewing NOTIFY previewingChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool lastOk READ lastOk NOTIFY statusChanged)

public:
    static RetargetController* instance();
    static RetargetController* qmlInstance(QQmlEngine*, QJSEngine*);
    static void kill();

    QStringList skeletalEntities() const { return m_entities; }
    bool canRetarget() const { return !m_entities.isEmpty(); }
    QString sourceEntity() const { return m_source; }
    QString targetEntity() const { return m_target; }
    QStringList sourceAnimations() const;
    QString sourceAnimation() const { return m_sourceAnim; }
    QStringList targetBones() const;
    QVariantList mappingRows() const;
    int mappedCount() const { return int(m_map.pairs.size()); }
    QStringList bundledMaps() const { return Retarget::bundledBoneMapNames(); }
    QString translationMode() const { return Retarget::translationModeId(m_opts.translation); }
    bool alignDirections() const { return m_opts.alignDirections; }
    QString sourceRest() const { return Retarget::sourceRestId(m_opts.sourceRest); }
    QString newAnimationName() const { return m_newName; }
    bool previewing() const { return m_previewing; }
    QString status() const { return m_status; }
    bool lastOk() const { return m_lastOk; }

    void setSourceEntity(const QString& name);
    void setTargetEntity(const QString& name);
    void setSourceAnimation(const QString& name);
    void setTranslationMode(const QString& id);
    void setAlignDirections(bool on);
    void setSourceRest(const QString& id);
    void setNewAnimationName(const QString& name);

    /// Re-scan the scene for skeletal entities (also runs on scene changes).
    Q_INVOKABLE void refresh();
    /// The wizard opened: breadcrumb + refresh + preselect from the selection.
    Q_INVOKABLE void noteOpened();
    /// Replace the current map with an automatic one.
    Q_INVOKABLE void autoMap();
    /// Map `sourceBone` → `targetBone` ("" clears the row).
    Q_INVOKABLE void setPairTarget(const QString& sourceBone, const QString& targetBone);
    Q_INVOKABLE void clearMapping();
    Q_INVOKABLE bool loadBundledMap(const QString& name);
    /// File dialogs (Qt-rendered, like the VAT folder picker).
    Q_INVOKABLE bool loadBoneMapFile();
    Q_INVOKABLE bool saveBoneMapFile();
    /// Import a mesh/animation file and select its skeletal entity as source.
    Q_INVOKABLE bool importSourceFile();

    Q_INVOKABLE bool startPreview();
    Q_INVOKABLE void stopPreview();
    /// Retarget for real: a new clip on the target, one undo step.
    Q_INVOKABLE bool apply();

    // Non-dialog variants (tests / scripting).
    bool loadBoneMapFrom(const QString& path);
    bool saveBoneMapTo(const QString& path);
    const Retarget::BoneMap& boneMap() const { return m_map; }

signals:
    void entitiesChanged();
    void selectionChanged();
    void mappingChanged();
    void optionsChanged();
    void previewingChanged();
    void statusChanged();
    void applied(const QString& entity, const QString& animation);

private:
    explicit RetargetController(QObject* parent = nullptr);
    ~RetargetController() override;

    Ogre::Entity* entityByName(const QString& name) const;
    void setStatus(const QString& s, bool ok);
    std::string defaultNewName() const;
    bool rebuildPreview();
    void tickPreview();

    QStringList m_entities;
    QString m_source;
    QString m_target;
    QString m_sourceAnim;
    QString m_newName;
    Retarget::BoneMap m_map;
    Retarget::Options m_opts;
    QString m_status;
    bool m_lastOk = true;

    // preview session
    bool m_previewing = false;
    QTimer m_previewTimer;
    std::string m_previewAnim;
    Ogre::Vector3 m_targetOrigPos = Ogre::Vector3::ZERO;
    bool m_targetMoved = false;
    struct SavedState { bool enabled; bool loop; float time; };
    std::map<std::string, SavedState> m_savedSource, m_savedTarget;

    static RetargetController* s_instance;
};

#endif // RETARGETCONTROLLER_H
