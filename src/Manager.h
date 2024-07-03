/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------------
*/

#ifndef _Manager_h
#define _Manager_h


#include <Ogre.h>
#include <OgreException.h>
#include <OgreSingleton.h>
#include <stdexcept>
#include <QString>
#include <QStringList>
#include <QList>
#include <QObject>

#if OGRE_PLATFORM == OGRE_PLATFORM_WIN32
#define WIN32_LEAN_AND_MEAN
#include "windows.h"
#endif

#ifdef DEBUG
    #define mResourcesCfg   "resources_d.cfg"
    #define mPluginsCfg     "plugins_d.cfg"
#else
    #define mResourcesCfg   "resources.cfg"
    #define mPluginsCfg     "plugins.cfg"
#endif

class ViewportGrid;
class MainWindow;

class Manager : public QObject
{
    Q_OBJECT

public:
    static Manager* getSingleton(MainWindow* parent = nullptr);
    static void kill();

    Ogre::Root*         getRoot()               const;
    Ogre::SceneManager* getSceneMgr()           const;
    MainWindow*         getMainWindow()         const;
    ViewportGrid*       getViewportGrid()       const;

    void                CreateEmptyScene();

    Ogre::SceneNode*    addSceneNode(const QString &_name);
    Ogre::SceneNode*    addSceneNode(const QString &_name, const Ogre::Any& anything);
    Ogre::SceneNode*    getSceneNode(const QString &_name);
    Ogre::Entity*       createEntity(Ogre::SceneNode* const& sceneNode, const Ogre::MeshPtr& mesh);

    void                destroySceneNode(const QString & name);
    void                destroySceneNode(Ogre::SceneNode* node);
    void                destroyAllAttachedMovableObjects(Ogre::SceneNode* node);

    bool                hasSceneNode(const QString &_name);
    QList<Ogre::SceneNode *>&   getSceneNodes();

    QList<Ogre::Entity *>&   getEntities();

    bool                isForbiddenNodeName(const QString &_name);

    bool                hasAnimationName(Ogre::Entity *entity, const QString &_name);

    bool                isValidFileExtention(QString &_uri);
    QString             getValidFileExtention();

    void loadResources();
signals:
    void sceneNodeCreated(Ogre::SceneNode* const& newNode);
    void sceneNodeDestroyed(Ogre::SceneNode* const& node);
    void entityCreated(Ogre::Entity* const& newEntity);

private:
    Manager(MainWindow *parent);
    ~Manager();
    void initRoot();         // Init Ogre Root
    void initRenderSystem(); // Init Ogre Render System
    void initSceneMgr();     // Init Ogre SceneManager

private:
    static Manager*      m_pSingleton; // the only instance of this!
    Ogre::Root*          mRoot;
    Ogre::SceneManager*  mSceneMgr;

    MainWindow*          m_pMainWindow;
    ViewportGrid*        m_pViewportGrid;

    Ogre::Plane*         mPlane;
    static QString       mValidFileExtention;

    QList<Ogre::SceneNode *> mSceneNodesList;
    QList<Ogre::Entity *> mEntitiesList;
};

#endif
