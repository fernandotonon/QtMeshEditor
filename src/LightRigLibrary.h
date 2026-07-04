#pragma once

#include "LightManager.h"

#include <OgreColourValue.h>

#include <QString>
#include <QStringList>

namespace Ogre
{
class SceneNode;
}

struct RemovedRigGroupSnapshot
{
    QString groupBaseName;
    QList<LightSnapshot> lights;
};

struct LightRigApplyResult
{
    bool ok = false;
    QString error;
    QString rigId;
    QString rigGroupNodeName;
    QList<LightSnapshot> addedLights;
    QList<RemovedRigGroupSnapshot> removedRigGroups;
    QList<LightSnapshot> removedUserLights;
    QList<LightSnapshot> removedLights;
    Ogre::ColourValue ambientBefore = Ogre::ColourValue::Black;
    Ogre::ColourValue ambientAfter = Ogre::ColourValue::Black;
    QString hdriBefore;
    QString suggestedHdri;
    bool replaceExisting = false;
};

namespace LightRigLibrary
{
inline constexpr const char* kRigGroupTag = "light_rig_group";

QStringList rigIds();
QString displayNameForId(const QString& id);
int indexOfRig(const QString& id);

QString defaultRigId();
QString readDefaultRigId();
void setDefaultRigId(const QString& id);

LightRigApplyResult apply(const QString& rigId, bool replaceExisting);
void applyDefaultSceneLighting();

void destroyAllRigGroups();
Ogre::SceneNode* createRigGroupForRig(const QString& rigId);
void tagRigGroupNode(Ogre::SceneNode* node);

bool sceneNodeIsRigGroup(Ogre::SceneNode* node);
void destroyRigGroupNode(const QString& nodeName);

} // namespace LightRigLibrary
