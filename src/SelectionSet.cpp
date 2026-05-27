#include <QtDebug>

#include <Ogre.h>
#include <QSet>

#include "Euler.h"
#include "Manager.h"
#include "SelectionSet.h"

namespace {

/// Collect Entity attachments on `node` and all descendant scene nodes
/// (skips internal/editor nodes via Manager::isForbiddenNodeName).
static void collectEntitiesOnNodeSubtree(Ogre::SceneNode* node, QList<Ogre::Entity*>& out,
                                         QSet<Ogre::Entity*>& seen)
{
    if (!node)
        return;

    for (unsigned short i = 0; i < node->numAttachedObjects(); ++i) {
        Ogre::MovableObject* obj = node->getAttachedObject(i);
        if (!obj || obj->getMovableType() != "Entity")
            continue;
        auto* ent = static_cast<Ogre::Entity*>(obj);
        if (!seen.contains(ent)) {
            seen.insert(ent);
            out.append(ent);
        }
    }

    for (Ogre::Node* child : node->getChildren()) {
        auto* childSn = static_cast<Ogre::SceneNode*>(child);
        const QString name = QString::fromStdString(childSn->getName());
        if (Manager::getSingleton()->isForbiddenNodeName(name))
            continue;
        collectEntitiesOnNodeSubtree(childSn, out, seen);
    }
}

} // namespace

////////////////////////////////////////
// Static variable initialisation
SelectionSet* SelectionSet:: m_pSingleton = nullptr;

////////////////////////////////////////
/// Static Member to build & destroy

SelectionSet* SelectionSet::getSingleton()
{
    if (m_pSingleton == nullptr)
        m_pSingleton = new SelectionSet();
    m_pSingleton->tryConnectToManager();
    return m_pSingleton;
}

SelectionSet *SelectionSet::getSingletonPtr()
{
    return m_pSingleton;
}

void SelectionSet::kill()
{
    if (m_pSingleton != nullptr)
    {
        delete m_pSingleton;
        m_pSingleton = nullptr;
    }
}

////////////////////////////////////////
// Constructor & Destructor

SelectionSet::SelectionSet()
    : QObject(nullptr)
{
}

void SelectionSet::tryConnectToManager()
{
    if (m_connectedToManager)
        return;
    auto *mgr = Manager::getSingletonPtr();
    if (!mgr)
        return;
    connect(mgr, &Manager::sceneNodeDestroyed, this, &SelectionSet::onSceneNodeDestroyed);
    m_connectedToManager = true;
}

void SelectionSet::onSceneNodeDestroyed(Ogre::SceneNode *node)
{
    if (!node)
        return;

    bool changed = false;
    const auto pruneNode = [&](auto &&self, Ogre::SceneNode *current) -> void {
        if (!current)
            return;

        if (mNodesSelected.removeOne(current) > 0)
            changed = true;

        for (unsigned short i = 0; i < current->numAttachedObjects(); ++i) {
            Ogre::MovableObject *obj = current->getAttachedObject(i);
            if (!obj || obj->getMovableType() != "Entity")
                continue;
            auto *ent = static_cast<Ogre::Entity *>(obj);
            if (mEntitiesSelected.removeOne(ent) > 0)
                changed = true;
            for (unsigned short s = 0; s < ent->getNumSubEntities(); ++s) {
                if (mSubEntitiesSelected.removeOne(ent->getSubEntity(s)) > 0)
                    changed = true;
            }
        }

        for (Ogre::Node *child : current->getChildren())
            self(self, static_cast<Ogre::SceneNode *>(child));
    };
    pruneNode(pruneNode, node);

    if (!changed)
        return;

    emit nodeSelectionChanged();
    emit entitySelectionChanged();
    emit subEntitySelectionChanged();
    emit selectionChanged();
}

void SelectionSet::append(Ogre::SceneNode* const& obj)
{
    if(!mNodesSelected.contains(obj))
    {
        obj->showBoundingBox(true);
        mNodesSelected.append(obj);
    }

    emit nodeSelectionChanged();
    emit selectionChanged();
}

void SelectionSet::append(Ogre::Entity* const& obj)
{
    if(!mEntitiesSelected.contains(obj))
    {
        obj->getParentSceneNode()->showBoundingBox(true);
        mEntitiesSelected.append(obj);
    }

    emit entitySelectionChanged();
    emit selectionChanged();
}

void SelectionSet::append(Ogre::SubEntity* const& obj)
{
    if(!mSubEntitiesSelected.contains(obj))
    {
        obj->getParent()->getParentSceneNode()->showBoundingBox(true);
        mSubEntitiesSelected.append(obj);
    }
    emit subEntitySelectionChanged();
    emit selectionChanged();
}



bool SelectionSet::removeOne(Ogre::SceneNode* const& obj)
{
    bool ok = false;
    ok = mNodesSelected.removeOne(obj);
    if(ok)
        hideBoundingBox(obj);
    emit nodeSelectionChanged();
    emit selectionChanged();
    return ok;
}

bool SelectionSet::removeOne(Ogre::Entity* const& obj)
{
    bool ok = false;
    ok = mEntitiesSelected.removeOne(obj);
    if(ok)
        hideBoundingBox(obj->getParentSceneNode());
    emit entitySelectionChanged();
    emit selectionChanged();
    return ok;
}

bool SelectionSet::removeOne(Ogre::SubEntity* const& obj)
{
    bool ok = false;
    ok = mSubEntitiesSelected.removeOne(obj);
    if(ok)
        hideBoundingBox(obj->getParent()->getParentSceneNode());
    emit subEntitySelectionChanged();
    emit selectionChanged();
    return ok;
}

void SelectionSet::selectOne(Ogre::SceneNode* const& obj)
{
    hideAllBoundingBox();

    mNodesSelected.clear();
    mEntitiesSelected.clear();
    mSubEntitiesSelected.clear();

    obj->showBoundingBox(true);
    mNodesSelected.append(obj);

    emit nodeSelectionChanged();
    emit selectionChanged();
}

void SelectionSet::selectOne(Ogre::Entity* const& obj)
{
    hideAllBoundingBox();

    mNodesSelected.clear();
    mEntitiesSelected.clear();
    mSubEntitiesSelected.clear();

    obj->getParentSceneNode()->showBoundingBox(true);
    mEntitiesSelected.append(obj);

    emit entitySelectionChanged();
    emit selectionChanged();
}

void SelectionSet::selectOne(Ogre::SubEntity* const& obj)
{
    hideAllBoundingBox();

    mNodesSelected.clear();
    mEntitiesSelected.clear();
    mSubEntitiesSelected.clear();

    obj->getParent()->getParentSceneNode()->showBoundingBox(true);
    mSubEntitiesSelected.append(obj);

    emit subEntitySelectionChanged();
    emit selectionChanged();
}

void SelectionSet::clear(void)
{
    hideAllBoundingBox();

    mNodesSelected.clear();
    mEntitiesSelected.clear();
    mSubEntitiesSelected.clear();


    emit nodeSelectionChanged();
    emit entitySelectionChanged();
    emit subEntitySelectionChanged();
    emit selectionChanged();
}

void SelectionSet::clearList(void)
{
    mNodesSelected.clear();
    mEntitiesSelected.clear();
    mSubEntitiesSelected.clear();

    emit nodeSelectionChanged();
    emit entitySelectionChanged();
    emit subEntitySelectionChanged();
    emit selectionChanged();
}

void SelectionSet::setEntityScaleFactor(Ogre::Entity *obj, const Ogre::Vector3 &scaleFactor)
{
    if(mEntityScaleFactor.count(obj))
    {
        mEntityScaleFactor[obj] = scaleFactor;
        return;
    }

    mEntityScaleFactor.insert(obj,scaleFactor);
}

Ogre::Vector3 SelectionSet::getEntityScaleFactor(Ogre::Entity *obj)
{
    if(mEntityScaleFactor.count(obj))
    {
        return mEntityScaleFactor.value(obj);
    }

    return Ogre::Vector3::UNIT_SCALE;
}

void SelectionSet::setEntityRotation(Ogre::Entity *obj, const Ogre::Vector3 &rotation)
{
    if(mEntityRotation.count(obj))
    {
        mEntityRotation[obj] = rotation;
        return;
    }

    mEntityRotation.insert(obj,rotation);
}

Ogre::Vector3 SelectionSet::getEntityRotation(Ogre::Entity *obj)
{
    if(mEntityRotation.count(obj))
    {
        return mEntityRotation.value(obj);
    }

    return Ogre::Vector3::ZERO;
}

Ogre::SceneNode* const& SelectionSet::getSceneNode(int i) const
{   return mNodesSelected.at(i); }

Ogre::Entity* const& SelectionSet::getEntity(int i) const
{   return mEntitiesSelected.at(i); }

Ogre::SubEntity* const& SelectionSet::getSubEntity(int i) const
{   return mSubEntitiesSelected.at(i); }

int SelectionSet::getNodesCount(void)         const
{    return  mNodesSelected.count(); }

int SelectionSet::getEntitiesCount(void)      const
{    return  mEntitiesSelected.count(); }

int SelectionSet::getSubEntitiesCount(void)   const
{    return  mSubEntitiesSelected.count(); }

int SelectionSet::getCount(void)  const
{    return  getNodesCount()
            + getEntitiesCount()
            + getSubEntitiesCount(); }

bool SelectionSet::contains(Ogre::SceneNode* const& obj) const
{   return  mNodesSelected.contains(obj); }

bool SelectionSet::contains(Ogre::Entity* const& obj) const
{   return  mEntitiesSelected.contains(obj); }

bool SelectionSet::contains(Ogre::SubEntity* const& obj) const
{   return  mSubEntitiesSelected.contains(obj); }

bool SelectionSet::hasNodes       (void)  const
{    return !mNodesSelected.isEmpty();    }

bool SelectionSet::hasEntities    (void)  const
{    return !mEntitiesSelected.isEmpty();    }

bool SelectionSet::hasSubEntities (void)  const
{    return !mSubEntitiesSelected.isEmpty();    }

bool SelectionSet::isEmpty()  const
{    return mNodesSelected.isEmpty()
            && mEntitiesSelected.isEmpty()
            && mSubEntitiesSelected.isEmpty();    }

const Ogre::Vector3 SelectionSet::getSelectionCenter(void)   const
{
    Ogre::Vector3 vResult = Ogre::Vector3::ZERO;

    if(hasNodes())
    {
        foreach(Ogre::SceneNode* obj, mNodesSelected)
            vResult += obj->getPosition();

        vResult = vResult/getNodesCount();
    }
    else if(hasEntities())
    {
        //Return the entity to mesh relative position
        foreach(Ogre::Entity* obj, mEntitiesSelected)
        {
            const Ogre::AxisAlignedBox boundingBox = obj->getWorldBoundingBox(true);
            if (boundingBox.isFinite())
                vResult += (boundingBox.getCenter() - Ogre::Vector3(0, boundingBox.getHalfSize().y, 0));
            else
                vResult += obj->getParentSceneNode()->getPosition();
        }

        vResult = vResult/getEntitiesCount();
    }
    else if(hasSubEntities())
    {
        //Return the entity to mesh relative position
        foreach(Ogre::SubEntity* obj, mSubEntitiesSelected)
        {
            const Ogre::AxisAlignedBox boundingBox = obj->getParent()->getWorldBoundingBox(true);
            if (boundingBox.isFinite())
                vResult += (boundingBox.getCenter() - Ogre::Vector3(0, boundingBox.getHalfSize().y, 0));
            else
                vResult += obj->getParent()->getParentSceneNode()->getPosition();
        }

        vResult = vResult/getSubEntitiesCount();
    }

    return (vResult);
}

const Ogre::Vector3 SelectionSet::getSelectionNodesCenter() const
{
    Ogre::Vector3 vResult = Ogre::Vector3::ZERO;

    if(hasNodes())
    {
        foreach(Ogre::SceneNode* obj, mNodesSelected)
            vResult += obj->getPosition();

        vResult = vResult/getNodesCount();
    }
    else if(hasEntities())
    {
        foreach(Ogre::Entity* obj, mEntitiesSelected)
            vResult += obj->getParentSceneNode()->getPosition();

        vResult = vResult/getEntitiesCount();
    }
    else if(hasSubEntities())
    {
        foreach(Ogre::SubEntity* obj, mSubEntitiesSelected)
            vResult += obj->getParent()->getParentSceneNode()->getPosition();

        vResult = vResult/getSubEntitiesCount();
    }

    return (vResult);
}

const Ogre::Vector3 SelectionSet::getSelectionScale(void)   const
{
    Ogre::Vector3 vResult = Ogre::Vector3::ZERO;

    if(hasNodes())
    {
        foreach(Ogre::SceneNode* obj, mNodesSelected)
            vResult += obj->getScale();

        vResult = vResult/getNodesCount();
    }
    else if(hasEntities())
    {
        //Return the entity to mesh relative position
        foreach(Ogre::Entity* obj, mEntitiesSelected)
            vResult += getSingleton()->getEntityScaleFactor(obj);

        vResult = vResult/getEntitiesCount();
    }

    return (vResult);
}

const Ogre::Vector3 SelectionSet::getSelectionOrientation(void)   const
{
    Ogre::Vector3 vResult = Ogre::Vector3::ZERO;

    if(hasNodes())
    {
        Ogre::Euler euler;
        foreach(Ogre::SceneNode* obj, mNodesSelected)
        {
            euler.fromQuaternion(obj->getOrientation());
            Ogre::Vector3 nodeOrientation(euler.pitch().valueDegrees(),
                                euler.yaw().valueDegrees(),
                                euler.roll().valueDegrees());

            vResult = vResult + nodeOrientation;
        }

        vResult = vResult/getNodesCount();
    }
    else if(hasEntities())
    {
        //Return the entity to mesh relative position
        foreach(Ogre::Entity* obj, mEntitiesSelected)
            vResult += getSingleton()->getEntityRotation(obj);

        vResult = vResult/getEntitiesCount();
    }

    return (vResult);
}

const QList<Ogre::SceneNode*>   SelectionSet::getNodesSelectionList()  const
{   return mNodesSelected;   }

const QList<Ogre::Entity*>      SelectionSet::getEntitiesSelectionList()      const
{   return mEntitiesSelected;   }

const QList<Ogre::SubEntity*>   SelectionSet::getSubEntitiesSelectionList()   const
{   return mSubEntitiesSelected;   }

QList<Ogre::Entity*> SelectionSet::getResolvedEntities() const
{
    if (hasEntities())
        return getEntitiesSelectionList();

    if (hasSubEntities()) {
        QList<Ogre::Entity*> fromSubs;
        QSet<Ogre::Entity*> seen;
        for (Ogre::SubEntity* sub : getSubEntitiesSelectionList()) {
            if (!sub)
                continue;
            Ogre::Entity* ent = sub->getParent();
            if (ent && !seen.contains(ent)) {
                seen.insert(ent);
                fromSubs.append(ent);
            }
        }
        if (!fromSubs.isEmpty())
            return fromSubs;
    }

    QList<Ogre::Entity*> entities;
    if (!hasNodes())
        return entities;

    QSet<Ogre::Entity*> seen;
    Ogre::SceneManager* sceneMgr = Manager::getSingleton()->getSceneMgr();
    for (Ogre::SceneNode* node : getNodesSelectionList()) {
        if (!node)
            continue;
        // Legacy layout: entity registered under the same name as the node.
        if (sceneMgr && sceneMgr->hasEntity(node->getName())) {
            Ogre::Entity* ent = sceneMgr->getEntity(node->getName());
            if (ent && !seen.contains(ent)) {
                seen.insert(ent);
                entities.append(ent);
            }
        }
        collectEntitiesOnNodeSubtree(node, entities, seen);
    }
    return entities;
}

void SelectionSet::hideBoundingBox(Ogre::SceneNode* node)  const
{
    if(mNodesSelected.contains(node))
        return;
    foreach(Ogre::Entity* entity, mEntitiesSelected)
        if(entity->getParentSceneNode() == node)
            return;
    foreach(Ogre::SubEntity* subEntiy, mSubEntitiesSelected)
        if(subEntiy->getParent()->getParentSceneNode() == node)
            return;
    node->showBoundingBox(false);
}

void SelectionSet::hideAllBoundingBox() const
{
    Manager *mgr = Manager::getSingletonPtr();
    Ogre::SceneManager *sceneMgr = mgr ? mgr->getSceneMgr() : nullptr;

    auto nodeStillLive = [&](Ogre::SceneNode *node) {
        if (!node || !sceneMgr)
            return false;
        try {
            return node->getCreator() == sceneMgr;
        } catch (...) {
            return false;
        }
    };

    for (Ogre::SceneNode *node : mNodesSelected) {
        if (nodeStillLive(node))
            node->showBoundingBox(false);
    }
    for (Ogre::Entity *entity : mEntitiesSelected) {
        if (!entity)
            continue;
        try {
            if (Ogre::SceneNode *parent = entity->getParentSceneNode()) {
                if (nodeStillLive(parent))
                    parent->showBoundingBox(false);
            }
        } catch (...) {
        }
    }
    for (Ogre::SubEntity *subEntity : mSubEntitiesSelected) {
        if (!subEntity)
            continue;
        try {
            Ogre::Entity *parentEnt = subEntity->getParent();
            if (!parentEnt)
                continue;
            if (Ogre::SceneNode *parentNode = parentEnt->getParentSceneNode()) {
                if (nodeStillLive(parentNode))
                    parentNode->showBoundingBox(false);
            }
        } catch (...) {
        }
    }
}
