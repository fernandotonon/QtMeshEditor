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
#include "RTShaderHelper.h"
#include "SentryReporter.h"
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

    shutdownRTShaderSystem();

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
        mSceneMgr->setAmbientLight(Ogre::ColourValue(0.3f, 0.3f, 0.3f));

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
    SentryReporter::addBreadcrumb("scene", "Destroy scene node");
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

    emit sceneNodeDestroyed(node);  // emitted before destruction so listeners can clean up while entities are still valid
    destroyAllAttachedMovableObjects(node);
    node->removeAndDestroyAllChildren();
    
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

   // Collect mesh/skeleton resource names before destroying entities
   std::vector<Ogre::String> meshNames;
   std::vector<Ogre::String> skeletonNames;

   try {
       auto attachedObjects = node->getAttachedObjects();

       for(auto attachedObject : attachedObjects)
       {
           try {
               if (attachedObject->getMovableType() == "Entity")
               {
                   auto* entity = static_cast<Ogre::Entity*>(attachedObject);
                   auto mesh = entity->getMesh();
                   if (mesh)
                   {
                       meshNames.push_back(mesh->getName());
                       if (!mesh->getSkeletonName().empty())
                           skeletonNames.push_back(mesh->getSkeletonName());
                   }
               }
               node->getCreator()->destroyMovableObject(attachedObject);
           } catch (...) {
               // Ignore exceptions during cleanup
           }
       }
   } catch (...) {
       // Ignore exceptions during cleanup
   }

   // Remove mesh/skeleton resources no longer in use by any entity
   for (const auto& name : meshNames)
   {
       try {
           auto mesh = Ogre::MeshManager::getSingleton().getByName(name);
           // use_count == 2 means only MeshManager + this local variable hold it
           if (mesh && mesh.use_count() <= 2)
               Ogre::MeshManager::getSingleton().remove(mesh);
       } catch (...) {}
   }
   for (const auto& name : skeletonNames)
   {
       try {
           auto skel = Ogre::SkeletonManager::getSingleton().getByName(name);
           if (skel && skel.use_count() <= 2)
               Ogre::SkeletonManager::getSingleton().remove(skel);
       } catch (...) {}
   }

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
    Ogre::RenderSystem *renderSystem = mRoot->getRenderSystemByName("OpenGL Rendering Subsystem");

    if (!renderSystem)
    {
        renderSystem = mRoot->getRenderSystemByName("OpenGL 3+ Rendering Subsystem");
    }

    if (!renderSystem)
    {
        OGRE_EXCEPT(Ogre::Exception::ERR_RENDERINGAPI_ERROR,
            "No OpenGL rendering subsystem found. Check that Ogre plugins are installed correctly.",
            "Manager::initRenderSystem");
    }

    mRoot->setRenderSystem( renderSystem );
    SentryReporter::setTag("ogre_renderer", QString::fromStdString(renderSystem->getName()));

    try {
        mRoot->saveConfig();
    } catch (const Ogre::Exception& e) {
        qWarning() << "saveConfig failed (non-critical):" << e.getFullDescription().c_str();
    }
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

    mSceneMgr = mRoot->createSceneManager();

    if (!mSceneMgr)
    {
        QMessageBox mBox;
        mBox.setText(QStringLiteral("Error: Failed to create SceneManager"));
        mBox.exec();
    }
}

void Manager::initRTShaderSystem()
{
    RTShaderHelper::initialize(mSceneMgr);
}

void Manager::shutdownRTShaderSystem()
{
    RTShaderHelper::shutdown(mSceneMgr);
}

void Manager::loadResources()
{
    // RTSS must be initialized after a render window (GL context) exists
    initRTShaderSystem();

    QString file = QCoreApplication::applicationDirPath();

    // Load resource paths from config file
    Ogre::ConfigFile cf;
    cf.load(QString(file+"/cfg/"+mResourcesCfg).toStdString());

    // Go through all sections & settings in the file
    const auto& seci = cf.getSettingsBySection();

    for (const auto& [secName, settings] : seci)
    {
        for (const auto& [typeName, archName] : settings)
        {
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

bool Manager::isValidFileExtention(const QString &_uri) const
{
    const auto extensions = mValidFileExtention.split(" ");
    for (const auto &ext : extensions)
        if (_uri.endsWith(ext, Qt::CaseInsensitive))
            return true;

    return false;
}

QString Manager::getValidFileExtention() const
{
    return mValidFileExtention;
}
