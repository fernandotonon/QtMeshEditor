/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "MorphAnimationManager.h"

#include "SelectionSet.h"
#include "SentryReporter.h"

#include <OgreAnimationState.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgrePose.h>

#include <algorithm>

MorphAnimationManager* MorphAnimationManager::s_instance = nullptr;

MorphAnimationManager* MorphAnimationManager::instance()
{
    if (!s_instance) s_instance = new MorphAnimationManager();
    return s_instance;
}

MorphAnimationManager* MorphAnimationManager::qmlInstance(QQmlEngine*, QJSEngine*)
{
    return instance();
}

void MorphAnimationManager::kill()
{
    if (!s_instance) return;
    delete s_instance;
    s_instance = nullptr;
}

MorphAnimationManager::MorphAnimationManager(QObject* parent) : QObject(parent)
{
    if (auto* sel = SelectionSet::getSingleton()) {
        connect(sel, &SelectionSet::selectionChanged,
                this, &MorphAnimationManager::morphTargetsChanged);
    }
}

MorphAnimationManager::~MorphAnimationManager() = default;

QStringList MorphAnimationManager::morphTargetsFor(Ogre::Entity* entity) const
{
    QStringList out;
    if (!entity) return out;
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return out;
    const auto& poseList = mesh->getPoseList();
    for (const Ogre::Pose* p : poseList) {
        if (!p) continue;
        const Ogre::String n = p->getName();
        if (!n.empty()) out << QString::fromStdString(n);
    }
    return out;
}

float MorphAnimationManager::weight(Ogre::Entity* entity, const QString& name) const
{
    if (!entity || name.isEmpty()) return 0.0f;
    auto* states = entity->getAllAnimationStates();
    if (!states) return 0.0f;
    const std::string sn = name.toStdString();
    if (!states->hasAnimationState(sn)) return 0.0f;
    auto* state = states->getAnimationState(sn);
    return state ? state->getWeight() : 0.0f;
}

bool MorphAnimationManager::setWeight(Ogre::Entity* entity, const QString& name, float w)
{
    if (!entity || name.isEmpty()) return false;
    auto* states = entity->getAllAnimationStates();
    if (!states) return false;
    const std::string sn = name.toStdString();
    if (!states->hasAnimationState(sn)) return false;
    auto* state = states->getAnimationState(sn);
    if (!state) return false;

    const float clamped = std::clamp(w, 0.0f, 1.0f);
    const float current = state->getWeight();
    const bool wasEnabled = state->getEnabled();
    if (std::abs(clamped - current) < 1e-6f && wasEnabled) return true;

    state->setEnabled(true);
    state->setWeight(clamped);
    // The pose track has its only keyframe at t=0; pin the state
    // there so the weight actually drives the pose.
    state->setTimePosition(0.0f);

    SentryReporter::addBreadcrumb("scene.anim.morph",
        QStringLiteral("set '%1' weight = %2").arg(name).arg(clamped, 0, 'f', 3));

    emit morphWeightChanged(entity, name, static_cast<double>(clamped));
    return true;
}

QStringList MorphAnimationManager::morphTargetsForSelection() const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return {};
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty()) return {};
    return morphTargetsFor(ents.first());
}

double MorphAnimationManager::weightForSelection(const QString& name) const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return 0.0;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty()) return 0.0;
    return static_cast<double>(weight(ents.first(), name));
}

bool MorphAnimationManager::setWeightForSelection(const QString& name, double w)
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    auto ents = sel->getResolvedEntities();
    if (ents.isEmpty()) return false;
    return setWeight(ents.first(), name, static_cast<float>(w));
}
