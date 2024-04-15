#include <QtDebug>

#include <Ogre.h>

#include "Euler.h"
#include "SelectionSet.h"

////////////////////////////////////////
// Static variable initialisation
SelectionSet* SelectionSet:: m_pSingleton = 0;

////////////////////////////////////////
/// Static Member to build & destroy

SelectionSet* SelectionSet::getSingleton()
{
  if (m_pSingleton == 0)
  {
      m_pSingleton =  new SelectionSet();
  }

  return m_pSingleton;
}

void SelectionSet::kill()
{
    if (m_pSingleton != 0)
    {
        delete m_pSingleton;
        m_pSingleton = 0;
    }
}

////////////////////////////////////////
// Constructor & Destructor

SelectionSet::SelectionSet()
    :QObject(nullptr)
{

}

SelectionSet::~SelectionSet()
{

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
            vResult += (obj->getWorldBoundingBox().getCenter() - Ogre::Vector3(0,obj->getWorldBoundingBox().getHalfSize().y,0));

        vResult = vResult/getEntitiesCount();
    }
    else if(hasSubEntities())
    {
        //Return the entity to mesh relative position
        foreach(Ogre::SubEntity* obj, mSubEntitiesSelected)
            vResult += (obj->getParent()->getWorldBoundingBox().getCenter() - Ogre::Vector3(0,obj->getParent()->getWorldBoundingBox().getHalfSize().y,0));

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

void SelectionSet::hideAllBoundingBox()  const
{
    foreach(Ogre::SceneNode* node, mNodesSelected)
        node->showBoundingBox(false);
    foreach(Ogre::Entity* entity, mEntitiesSelected)
        entity->getParentSceneNode()->showBoundingBox(false);
    foreach(Ogre::SubEntity* subEntiy, mSubEntitiesSelected)
        subEntiy->getParent()->getParentSceneNode()->showBoundingBox(false);
}
