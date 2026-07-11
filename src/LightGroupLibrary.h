#pragma once

#include "LightManager.h"

#include <QString>
#include <QStringList>

namespace Ogre
{
class SceneNode;
}

struct LightGroupCreateResult
{
    bool ok = false;
    QString error;
    QString groupNodeName;
    QStringList movedLightNames;
};

namespace LightGroupLibrary
{

inline constexpr const char* kLightGroupTag = "light_group";

bool sceneNodeIsLightGroup(Ogre::SceneNode* node);
void tagLightGroupNode(Ogre::SceneNode* node);

QStringList listLightGroupNames();

LightGroupCreateResult createGroup(const QString& groupName, const QStringList& lightNames);
bool dissolveGroup(const QString& groupNodeName);
bool setGroupEnabled(const QString& groupNodeName, bool enabled);
bool renameGroup(const QString& oldName, const QString& newName);

} // namespace LightGroupLibrary
