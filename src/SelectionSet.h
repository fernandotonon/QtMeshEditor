#ifndef SELECTION_SET_H
#define SELECTION_SET_H

#include <QObject>
#include <QList>
#include <QMap>

#include <Ogre.h>
namespace Ogre
{    class SceneNode;   }

class SelectionSet : public QObject
{
    Q_OBJECT

public :
    static SelectionSet* getSingleton();
    static SelectionSet* getSingletonPtr();
    static void kill();
    /** Connect to `Manager::sceneNodeDestroyed` when a Manager exists.
     *  Called from `getSingleton()` and from `Manager`'s constructor so
     *  tests that recreate SelectionSet without Manager do not boot Ogre. */
    void tryConnectToManager();

private:
    SelectionSet();
    ~SelectionSet(void) override = default;

public :
    void append(Ogre::SceneNode* const& obj);
    bool removeOne(Ogre::SceneNode* const& obj);
    void selectOne(Ogre::SceneNode* const& obj);

    void append(Ogre::Entity* const& obj);
    bool removeOne(Ogre::Entity* const& obj);
    void selectOne(Ogre::Entity* const& obj);

    void append(Ogre::SubEntity* const& obj);
    bool removeOne(Ogre::SubEntity* const& obj);
    void selectOne(Ogre::SubEntity* const& obj);

    void clear(void);
    void clearList(void);   //use this one is items in the list has been destroyed
                            //before calling clear() method

    void setEntityScaleFactor(Ogre::Entity* obj, const Ogre::Vector3 &scaleFactor);
    Ogre::Vector3 getEntityScaleFactor(Ogre::Entity *obj);

    void setEntityRotation(Ogre::Entity* obj, const Ogre::Vector3 &rotation);
    Ogre::Vector3 getEntityRotation(Ogre::Entity *obj);

    Ogre::SceneNode*    const& getSceneNode (int i) const;
    Ogre::Entity*       const& getEntity    (int i) const;
    Ogre::SubEntity*    const& getSubEntity (int i) const;

    int getNodesCount(void)         const;
    int getEntitiesCount(void)      const;
    int getSubEntitiesCount(void)   const;
    int getCount(void)              const;

    bool contains(Ogre::SceneNode* const& obj) const;
    bool contains(Ogre::Entity*    const& obj) const;
    bool contains(Ogre::SubEntity* const& obj) const;

    bool hasNodes       (void)  const;
    bool hasEntities    (void)  const;
    bool hasSubEntities (void)  const;

    bool isEmpty(void)  const;

    const Ogre::Vector3             getSelectionCenter(void)        const;
    const Ogre::Vector3             getSelectionNodesCenter(void)   const;
    const Ogre::Vector3             getSelectionOrientation(void)   const;
    const Ogre::Vector3             getSelectionScale(void)         const;

    const QList<Ogre::SceneNode*>   getNodesSelectionList()          const;
    const QList<Ogre::Entity*>      getEntitiesSelectionList()      const;
    const QList<Ogre::SubEntity*>   getSubEntitiesSelectionList()   const;

    /// Returns entities from the current selection. Direct entity picks win;
    /// otherwise submesh picks resolve to their parent entities; otherwise
    /// selected scene nodes resolve to entities attached on that node or any
    /// descendant (skips internal node names via Manager::isForbiddenNodeName),
    /// plus the legacy case where SceneManager registers an entity under the
    /// same name as the node.
    QList<Ogre::Entity*> getResolvedEntities() const;
private:
    void hideBoundingBox(Ogre::SceneNode* node)  const;
    void hideAllBoundingBox()  const;
    /** Drop any selection entries tied to `node` or its attached
     *  entities. Connected to `Manager::sceneNodeDestroyed` so a fresh
     *  PS1 capture (which destroys `PS1Capture_*` nodes) cannot leave
     *  dangling pointers that crash the next `selectOne` / promote. */
    void onSceneNodeDestroyed(Ogre::SceneNode *node);
signals:
    void selectionChanged();
    void nodeSelectionChanged();
    void entitySelectionChanged();
    void subEntitySelectionChanged();

private :
    static SelectionSet*    m_pSingleton; // the only instance of this!
    bool m_connectedToManager = false;
    QList<Ogre::SceneNode*> mNodesSelected;
    QList<Ogre::Entity*>    mEntitiesSelected;
    QList<Ogre::SubEntity*> mSubEntitiesSelected;
    QMap<Ogre::Entity*, Ogre::Vector3> mEntityScaleFactor;
    QMap<Ogre::Entity*, Ogre::Vector3> mEntityRotation;
};

#endif //SELECTION_SET_H

