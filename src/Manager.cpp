/*/////////////////////////////////////////////////////////////////////////////////
/// A QtMeshEditor file
///
/// Copyright (c) HogPog Team (www.hogpog.com.br)
///
/// The MIT License
///
/// Permission is hereby granted, free of charge, to any person obtaining a copy
/// of this software and associated documentation files (the "Software"), to deal
/// in the Software without restriction, including without limitation the rights
/// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
/// copies of the Software, and to permit persons to whom the Software is
/// furnished to do so, subject to the following conditions:
///
/// The above copyright notice and this permission notice shall be included in
/// all copies or substantial portions of the Software.
///
/// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
/// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
/// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
/// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
/// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
/// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
/// THE SOFTWARE.
////////////////////////////////////////////////////////////////////////////////*/
#include <QCoreApplication>
#include <QMessageBox>

#include "GlobalDefinitions.h"

#include "PrimitiveObject.h"

#include "Manager.h"
#include "SelectionSet.h"
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
Manager* Manager:: m_pSingleton = 0;

QString Manager::mValidFileExtention = ".mesh .dae .blend .3ds .ase .obj .ifc .xgl .zgl .ply .dxf .lwo "\
        ".lws .lxo .stl .x .ac .ms3d .cob .scn .bvh .csm .xml .irrmesh .irr .mdl .md2 .md3 "\
        ".pk3 .mdc .md5 .txt .smd .vta .m3 .3d .b3d .q3d .q3s .nff .nff .off .raw .ter .mdl .hmp .ndo .fbx .glb .gltf";

////////////////////////////////////////
/// Static Member to build & destroy

Manager* Manager::getSingleton(MainWindow* parent)
{
  if (m_pSingleton == 0)
  {
      assert(parent);
      m_pSingleton =  new Manager(parent);
  }

  return m_pSingleton;
}

void Manager::kill()
{
    if (m_pSingleton != 0)
    {
        delete m_pSingleton;
        m_pSingleton = 0;
    }
}

////////////////////////////////////////
// Constructor & Destructor

Manager::Manager(MainWindow* parent):
    mRoot(0), mSceneMgr(0), mPlane(0), m_pMainWindow(parent), m_pViewportGrid(0)/*, m_pSelectedNode(0)*/
{
    initRoot();         // Init Ogre Root
    initRenderSystem(); // Init Ogre Render System
    initSceneMgr();     // Init Ogre SceneManager
}
Manager::~Manager()
{
    if(m_pViewportGrid)
    {
        delete m_pViewportGrid;
        m_pViewportGrid = 0;
    }

    if (mPlane)
    {
        delete mPlane;
        mPlane = 0;
    }

    mSceneMgr->clearScene();
    mRoot->destroySceneManager(mSceneMgr);
    mSceneMgr = 0;

    mRoot->shutdown();
    delete mRoot;
    mRoot = 0;
}

void Manager::CreateEmptyScene()
{
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
}


Ogre::SceneNode* Manager::addSceneNode(const QString &_name)
{
    Ogre::SceneNode *sn = NULL;
    unsigned int number = 0;

    while(hasSceneNode(QString(_name+(number?QString::number(number):""))))
        ++number;

    sn = getSceneMgr()->getRootSceneNode()->createChildSceneNode(QString(_name+(number?QString::number(number):"")).toStdString());

    emit sceneNodeCreated(sn);
    SelectionSet::getSingleton()->selectOne(sn);
    return sn;
}

Ogre::SceneNode* Manager::addSceneNode(const QString &_name, const Ogre::Any& anything)
{
    Ogre::SceneNode *sn = NULL;
    unsigned int number = 0;

    while(hasSceneNode(QString(_name+(number?QString::number(number):""))))
        ++number;

    sn = getSceneMgr()->getRootSceneNode()->createChildSceneNode(QString(_name+(number?QString::number(number):"")).toStdString().data());
    sn->getUserObjectBindings().setUserAny(anything);

    emit sceneNodeCreated(sn);
    SelectionSet::getSingleton()->selectOne(sn);
    return sn;
}

Ogre::Entity* Manager::createEntity(Ogre::SceneNode* const& sceneNode, const Ogre::MeshPtr& mesh)
{
    Ogre::Entity* ent;

    ent = mSceneMgr->createEntity(sceneNode->getName(),mesh);

    sceneNode->attachObject(ent);
    emit entityCreated(ent);
    SelectionSet::getSingleton()->selectOne(sceneNode);
    return ent;
}

void Manager::destroySceneNode(const QString & name)
{
    destroySceneNode(mSceneMgr->getSceneNode(name.toStdString().data()));
}
void Manager::destroySceneNode(Ogre::SceneNode* node)
{
    if(!node)
        return;


    if(PrimitiveObject::isPrimitive(node))
    {
        PrimitiveObject* primitive = PrimitiveObject::getPrimitiveFromSceneNode(node);
        //if (!(node->getUserObjectBindings().getUserAny().isEmpty()))
            node->getUserObjectBindings().clear();

        delete primitive;
    }
    //TODO if custom class has to be provided for object, userany object should be inside so that this delete is not required...

    destroyAllAttachedMovableObjects(node);
    node->removeAndDestroyAllChildren();
    emit sceneNodeDestroyed(node);  //emitted just before destroying
    mSceneMgr->destroySceneNode(node);
}


void Manager::destroyAllAttachedMovableObjects(Ogre::SceneNode* node)
{
   if(!node)
       return;

   // Destroy all the attached objects
   auto attachedObjects = node->getAttachedObjects();

   for(auto attachedObject : attachedObjects)
      node->getCreator()->destroyMovableObject(attachedObject);
   /* TODO check to free up the meshmanager
   if(ent->getMesh().getPointer()->isManuallyLoaded())
   {
       pSceneMgr->destroyManualObject(currentName);
       Ogre::MeshManager::getSingleton().remove(currentName);
   }*/

   // Recurse to child SceneNodes
   auto children = node->getChildren();
   for(auto child : children)
   {
      Ogre::SceneNode* pChildNode = static_cast<Ogre::SceneNode*>(child);
      destroyAllAttachedMovableObjects( pChildNode );
   }
}

Ogre::SceneNode *Manager::getSceneNode(const QString &_name)
{
    return hasSceneNode(_name)?getSceneMgr()->getSceneNode(_name.toStdString().data()):NULL;
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
            ||_name.startsWith("Unnamed_")); //TODO find what is this <- Done, it's the cameras's nodes
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
        QString file = QCoreApplication::applicationDirPath();
        mRoot = new Ogre::Root(QString(file + "/cfg/" + mPluginsCfg ).toStdString().data()
                               , QString(file +"/cfg/Video.cfg").toStdString().data()
                                         , QString(file+"/cfg/Graphics.log").toStdString().data());
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
#if OGRE_PLATFORM == OGRE_PLATFORM_APPLE
            // OS X does not set the working directory relative to the app,
            // In order to make things portable on OS X we need to provide
            // the loading with it's own bundle path location
            Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
                Ogre::String(macBundlePath() + "/" + archName), typeName, secName);
#else
            Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
                archName, typeName, secName);
#endif
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



