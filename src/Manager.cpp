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

#include <QCoreApplication>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDir>

#include "GlobalDefinitions.h"

#include "PrimitiveObject.h"

#include "Manager.h"
#include "SelectionSet.h"
#include "TransformOperator.h"
#include "mainwindow.h"
#include "ViewportGrid.h"

#if OGRE_PLATFORM == OGRE_PLATFORM_APPLE
 #include <CoreFoundation/CoreFoundation.h>
 
 // This function will locate the path to our application on OS X,
 // unlike windows you cannot rely on the current working directory
 // for locating your configuration files and resources.
 std::string macBundlePath()
 {
     char path[1024];
     CFBundleRef mainBundle = CFBundleGetMainBundle();
     assert(mainBundle);
 
     CFURLRef mainBundleURL = CFBundleCopyBundleURL(mainBundle);
     assert(mainBundleURL);
 
     CFStringRef cfStringRef = CFURLCopyFileSystemPath( mainBundleURL, kCFURLPOSIXPathStyle);
     assert(cfStringRef);
 
     CFStringGetCString(cfStringRef, path, 1024, kCFStringEncodingASCII);
 
     CFRelease(mainBundleURL);
     CFRelease(cfStringRef);
 
     return std::string(path);
 }
 #endif

////////////////////////////////////////
// Static variable initialisation
Manager* Manager:: m_pSingleton = nullptr;

QString Manager::mValidFileExtention = ".mesh .dae .blend .3ds .ase .obj .ifc .xgl .zgl .ply .dxf .lwo "\
        ".lws .lxo .stl .x .ac .ms3d .cob .scn .bvh .csm .xml .irrmesh .irr .mdl .md2 .md3 "\
        ".pk3 .mdc .md5 .txt .smd .vta .m3 .3d .b3d .q3d .q3s .nff .nff .off .raw .ter .mdl .hmp .ndo .fbx .glb .gltf";

////////////////////////////////////////
/// Static Member to build & destroy

Manager* Manager::getSingleton(MainWindow* parent)
{
  if (m_pSingleton == nullptr)
  {
      assert(parent);
      m_pSingleton =  new Manager(parent);
  }

  return m_pSingleton;
}

Manager* Manager::getSingletonPtr()
{
    return m_pSingleton;
}

void Manager::kill()
{
    if (m_pSingleton != nullptr)
    {
        TransformOperator::kill();
        SelectionSet::kill();
        delete m_pSingleton;
        m_pSingleton = nullptr;
    }
}

////////////////////////////////////////
// Constructor & Destructor

Manager::Manager(MainWindow* parent):
    mRoot(nullptr), mSceneMgr(nullptr), mPlane(nullptr), m_pMainWindow(parent), m_pViewportGrid(nullptr)
{
    initRoot();         // Init Ogre Root
    initRenderSystem(); // Init Ogre Render System
    initSceneMgr();     // Init Ogre SceneManager
}
Manager::~Manager()
{
    // Clean up viewport grid first
    if(m_pViewportGrid)
    {
        delete m_pViewportGrid;
        m_pViewportGrid = nullptr;
    }

    // Clean up plane
    if (mPlane)
    {
        delete mPlane;
        mPlane = nullptr;
    }

    if (mSceneMgr)
    {
        try {
            mSceneMgr->clearScene();
        } catch (...) {
            // Ignore exceptions during shutdown
        }

        if (mRoot)
        {
            try {
                mRoot->destroySceneManager(mSceneMgr);
            } catch (...) {
                // Ignore exceptions during shutdown
            }
        }
        mSceneMgr = nullptr;
    }

    // Clear our reference lists
    mSceneNodesList.clear();
    mEntitiesList.clear();

    // Shutdown and destroy OGRE root
    // CRITICAL: Ensure proper shutdown order to avoid crashes when multiple tests run
    // The render targets should already be detached by OgreWidget destructors
    if (mRoot)
    {
        try {
            // Shutdown the root - this will clean up all render targets and resources
            // Note: Render targets should already be detached by OgreWidget destructors
            mRoot->shutdown();
        } catch (const Ogre::Exception& e) {
            // Log but continue - some resources may already be destroyed
            // This can happen when multiple tests run in sequence
        } catch (...) {
            // Ignore other exceptions during shutdown
        }
        
        try {
            delete mRoot;
        } catch (...) {
            // Ignore exceptions during deletion
        }
        mRoot = nullptr;
    }
}

void Manager::CreateEmptyScene()
{
    const bool previousState = mInitializingScene;
    mInitializingScene = true;

    { //TODO: Add the hability of the user adding/removing lights
        Ogre::Light* light = mSceneMgr->createLight();

        light->setType(Ogre::Light::LT_DIRECTIONAL);

        light->setDiffuseColour(1.0f, 1.0f, 1.0f);
        light->setSpecularColour(.8f, .8f, .8f);// color of 'reflected' light

        Ogre::SceneNode* lightSceneNode = mSceneMgr->getRootSceneNode()->createChildSceneNode();
        lightSceneNode->attachObject(light);
        lightSceneNode->setDirection(1, -1, 1);
    }

    m_pViewportGrid = new ViewportGrid();

    mInitializingScene = previousState;
}


Ogre::SceneNode* Manager::addSceneNode(const QString &_name)
{
    Ogre::SceneNode *sn = nullptr;
    unsigned int number = 0;

    while(hasSceneNode(QString(_name+(number?QString::number(number):""))))
        ++number;

    sn = getSceneMgr()->getRootSceneNode()->createChildSceneNode(QString(_name+(number?QString::number(number):"")).toStdString());

    emit sceneNodeCreated(sn);
    if(!mInitializingScene)
        SelectionSet::getSingleton()->selectOne(sn);
    return sn;
}

Ogre::SceneNode* Manager::addSceneNode(const QString &_name, const Ogre::Any& anything)
{
    Ogre::SceneNode *sn = nullptr;
    unsigned int number = 0;

    while(hasSceneNode(QString(_name+(number?QString::number(number):""))))
        ++number;

    sn = getSceneMgr()->getRootSceneNode()->createChildSceneNode(QString(_name+(number?QString::number(number):"")).toStdString().data());
    sn->getUserObjectBindings().setUserAny(anything);

    emit sceneNodeCreated(sn);
    if(!mInitializingScene)
        SelectionSet::getSingleton()->selectOne(sn);
    return sn;
}

Ogre::Entity* Manager::createEntity(Ogre::SceneNode* const& sceneNode, const Ogre::MeshPtr& mesh)
{
    Ogre::Entity* ent;

    ent = mSceneMgr->createEntity(sceneNode->getName(),mesh);

    sceneNode->attachObject(ent);
    emit entityCreated(ent);
    if(!mInitializingScene)
        SelectionSet::getSingleton()->selectOne(sceneNode);
    return ent;
}

void Manager::destroySceneNode(const QString & name)
{
    if (!mSceneMgr)
        return;
    
    try {
        Ogre::SceneNode* node = mSceneMgr->getSceneNode(name.toStdString().data());
        if (node)
        {
            destroySceneNode(node);
        }
    } catch (...) {
        // Node may not exist or scene manager may be shutting down
    }
}
void Manager::destroySceneNode(Ogre::SceneNode* node)
{
    if(!node || !mSceneMgr || isForbiddenNodeName(node->getName().c_str()))
        return;

    // Check if node still exists and belongs to our scene manager
    try {
        if (node->getCreator() != mSceneMgr)
            return;
    } catch (...) {
        // Node may already be destroyed or invalid
        return;
    }

    if(PrimitiveObject::isPrimitive(node))
    {
        PrimitiveObject* primitive = PrimitiveObject::getPrimitiveFromSceneNode(node);
        node->getUserObjectBindings().clear();

        delete primitive;
    }
    //TODO if custom class has to be provided for object, userany object should be inside so that this delete is not required...

    destroyAllAttachedMovableObjects(node);
    node->removeAndDestroyAllChildren();
    emit sceneNodeDestroyed(node);  //emitted just before destroying
    
    // Safely destroy the scene node
    try {
        mSceneMgr->destroySceneNode(node);
    } catch (...) {
        // Ignore exceptions during shutdown
    }
}


void Manager::destroyAllAttachedMovableObjects(Ogre::SceneNode* node)
{
   if(!node || !mSceneMgr)
       return;

   // Check if node is still valid
   try {
       if (node->getCreator() != mSceneMgr)
           return;
   } catch (...) {
       // Node may already be destroyed or invalid
       return;
   }

   // Destroy all the attached objects
   try {
       auto attachedObjects = node->getAttachedObjects();

       for(auto attachedObject : attachedObjects)
       {
           try {
               node->getCreator()->destroyMovableObject(attachedObject);
           } catch (...) {
               // Ignore exceptions during cleanup
           }
       }
   } catch (...) {
       // Ignore exceptions during cleanup
   }

   /* TODO check to free up the meshmanager
   if(ent->getMesh().getPointer()->isManuallyLoaded())
   {
       pSceneMgr->destroyManualObject(currentName);
       Ogre::MeshManager::getSingleton().remove(currentName);
   }*/

   // Recurse to child SceneNodes
   try {
       auto children = node->getChildren();
       for(auto child : children)
       {
          Ogre::SceneNode* pChildNode = static_cast<Ogre::SceneNode*>(child);
          destroyAllAttachedMovableObjects( pChildNode );
       }
   } catch (...) {
       // Ignore exceptions during cleanup
   }
}

Ogre::SceneNode *Manager::getSceneNode(const QString &_name)
{
    return hasSceneNode(_name) ? getSceneMgr()->getSceneNode(_name.toStdString().data()) : nullptr;
}

bool Manager::hasSceneNode(const QString &_name)
{
    auto children = getSceneMgr()->getRootSceneNode()->getChildren();
    for(auto node : children)
    {
        if(_name==node->getName().data())
            return true;
    }
    return false;
}

QList<Ogre::SceneNode *> &Manager::getSceneNodes()
{
    mSceneNodesList.clear();

    auto nodes = getSceneMgr()->getRootSceneNode()->getChildren();
    for(Ogre::Node* node : nodes)
    {
        Ogre::SceneNode* pSN = static_cast<Ogre::SceneNode*>(node);
        QString name = pSN->getName().data();
        if(!(isForbiddenNodeName(name)))
            mSceneNodesList.append(pSN);
    }

    return mSceneNodesList;
}

QList<Ogre::Entity *> &Manager::getEntities()
{
    mEntitiesList.clear();

    auto nodes = getSceneMgr()->getRootSceneNode()->getChildren();
    for(Ogre::Node* node : nodes)
    {
        Ogre::SceneNode* pSN = static_cast<Ogre::SceneNode*>(node);
        QString name = pSN->getName().data();
        if(!(isForbiddenNodeName(name)))
        {
            Ogre::SceneNode *parentNode = pSN;
            for(int entIndex = 0;  entIndex < parentNode->numAttachedObjects();entIndex++)
            {
                mEntitiesList.append(static_cast<Ogre::Entity*>(parentNode->getAttachedObject(entIndex)));
            }
        }
    }
    return mEntitiesList;
}

bool Manager::isForbiddenNodeName(const QString &_name)
{
    return (_name=="TPCameraChildSceneNode" //TODO add a define for TPCameraChildSceneNode
            ||_name=="GridLine_node" //TODO add a define for GridLine_node
            ||_name==SELECTIONBOX_OBJECT_NAME
            ||_name==TRANSFORM_OBJECT_NAME
            ||_name.startsWith("Unnamed_")); //This is the cameras's nodes
}

bool Manager::hasAnimationName(Ogre::Entity *entity, const QString &_name)
{
    if(!entity->hasSkeleton()) return false;
    auto animationStateSet = entity->getAllAnimationStates();

    return std::any_of(animationStateSet->getAnimationStates().begin(), animationStateSet->getAnimationStates().end(),
                       [&_name](const std::pair<std::string, Ogre::AnimationState*>& animState)
    {
        return _name.toStdString() == animState.second->getAnimationName();
    });
}


Ogre::SceneManager* Manager::getSceneMgr() const
{    return mSceneMgr;  }

Ogre::Root* Manager::getRoot() const
{    return mRoot;  }

MainWindow* Manager::getMainWindow() const
{    return m_pMainWindow;  }

ViewportGrid* Manager::getViewportGrid() const
{   return m_pViewportGrid;   }

void Manager::initRoot()
{
    try
    {
        QString appDir = QCoreApplication::applicationDirPath();
        
        // Use user-writable directory for config files (Video.cfg, Graphics.log)
        QString userConfigDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        QDir configDir(userConfigDir);
        if (!configDir.exists()) {
            configDir.mkpath(".");
        }
        
        // Plugins config is read from installation directory (read-only is fine)
        QString pluginsCfgPath = appDir + "/cfg/" + mPluginsCfg;
        
        // User-specific config files go to writable location
        QString videoCfgPath = userConfigDir + "/Video.cfg";
        QString logPath = userConfigDir + "/Graphics.log";
        
        qDebug() << "OGRE config paths:";
        qDebug() << "  Plugins:" << pluginsCfgPath;
        qDebug() << "  Video config:" << videoCfgPath;
        qDebug() << "  Log:" << logPath;
        
        mRoot = new Ogre::Root(pluginsCfgPath.toStdString().data(),
                               videoCfgPath.toStdString().data(),
                               logPath.toStdString().data());
        if (!mRoot)
        {
            throw std::logic_error("Erro: Iniciando Root\nFILE: "+std::string(__FILE__)+"\nLINE: "+QString::number(__LINE__).toStdString());
        }
    }
    catch (std::logic_error const& le)
    {
        QMessageBox mBox;
        mBox.setText(le.what());
        mBox.exec();
    }
}

void Manager::initRenderSystem()
{
    // setup a renderer
    Ogre::RenderSystem *renderSystem = mRoot->getRenderSystemByName("OpenGL Rendering Subsystem"); //TODO: Add OpenGL 3+, and allow the user to select the render system.

    assert( renderSystem ); // user might pass back a null renderer, which would be bad!

    mRoot->setRenderSystem( renderSystem );

  /*
    Ogre::ConfigOptionMap::iterator it = mOgreRoot->getRenderSystem()->getConfigOptions().begin();

    while(it != mOgreRoot->getRenderSystem()->getConfigOptions().end())
    {
        Ogre::ConfigOption p = (*it).second;
        qDebug()<<p.name.data()<<p.currentValue.data();
        ++it;
    }*/

    mRoot->saveConfig();
    mRoot->initialise(false); // don't create a window

    // All objects will be build on this flag
    Ogre::MovableObject::setDefaultQueryFlags(SCENE_QUERY_FLAGS);
}


void Manager::initSceneMgr()
{
    /* Akira
    Scene Manager should be created before render window,
    I notice that if not, issue with infinite Bounding boxes (I don't know why...)
    */

    try
    {
        mSceneMgr = mRoot->createSceneManager(/*"OctreeSceneManager"*/); //TODO: Creating with the default scene manager, verify if it would be good to change. Before it was using ST_EXTERIOR_CLOSE

        if (!mSceneMgr)
        {
            throw std::logic_error("Erro: Iniciando SceneManager\nFILE: "+std::string(__FILE__)+"\nLINE: "+QString::number(__LINE__).toStdString());
        }
    }
    catch (std::logic_error const& le)
    {
        QMessageBox mBox;
        mBox.setText(QString("Logic error - ")+le.what());
        mBox.exec();
    }
}

void Manager::loadResources()
{
    QString file = QCoreApplication::applicationDirPath();

    // Load resource paths from config file
    Ogre::ConfigFile cf;
    cf.load(QString(file+"/cfg/"+mResourcesCfg).toStdString().data());

    // Go through all sections & settings in the file
    auto seci = cf.getSettingsBySection();

    Ogre::String secName, typeName, archName;
    for(const auto &settingsPair : seci)
    {
        secName = settingsPair.first;
        Ogre::ConfigFile::SettingsMultiMap settings = static_cast<Ogre::ConfigFile::SettingsMultiMap>(settingsPair.second);
        Ogre::ConfigFile::SettingsMultiMap::iterator i;
        for (i = settings.begin(); i != settings.end(); ++i)
        {
            typeName = i->first;
            archName = i->second;
            // Resolve relative paths against the application directory so that
            // resources are found regardless of the current working directory
            // (e.g., when launched from an installed .deb package).
            QString archPath = QString::fromStdString(archName);
            if (!QDir::isAbsolutePath(archPath)) {
#if OGRE_PLATFORM == OGRE_PLATFORM_APPLE
                archPath = QString::fromStdString(macBundlePath()) + "/" + archPath;
#else
                archPath = file + "/" + archPath;
#endif
            }
            Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
                archPath.toStdString(), typeName, secName);
        }
    }

    Ogre::ResourceGroupManager::getSingleton().initialiseAllResourceGroups();

    // Material for all the GUI object
    Ogre::MaterialPtr matptr = Ogre::MaterialManager::getSingleton().getByName(GUI_MATERIAL_NAME, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    if(!matptr)
        matptr = Ogre::static_pointer_cast<Ogre::Material>(Ogre::MaterialManager::getSingleton().create(GUI_MATERIAL_NAME, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME));

    matptr->getTechnique(0)->setLightingEnabled(false);
    matptr->getTechnique(0)->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
    matptr->getTechnique(0)->setDepthCheckEnabled( false );  //IMPORTANT when setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY);
}
/*
Ogre::Plane &Manager::getGroundPlane()
{
    if(!mPlane)
        mPlane = new Ogre::Plane(Ogre::Vector3::UNIT_Y, 0);
    return *mPlane;
}
*/
bool Manager::isValidFileExtention(QString &_uri)
{
    for(int i = mValidFileExtention.split(" ").count()-1; i >= 0; --i)
        if(_uri.endsWith(mValidFileExtention.split(" ").at(i),Qt::CaseInsensitive))
            return true;

    return false;
}

QString Manager::getValidFileExtention()
{
    return mValidFileExtention;
}
