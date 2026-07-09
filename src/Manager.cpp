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

#include <vector>

#include "GlobalDefinitions.h"

#include "PrimitiveObject.h"

#include "Manager.h"
#include "RTShaderHelper.h"
#include <OgreSkeletonManager.h>
#include <OgreSkeleton.h>
#include <OgreBone.h>
#include <OgreAnimation.h>
#include "SentryReporter.h"
#include "SelectionSet.h"
#include "TransformOperator.h"
#include "mainwindow.h"
#include "ViewportGrid.h"
#include "HDR/HDREnvironmentManager.h"
#include "HDR/HdrBundledLibrary.h"
#include "HDR/HdrViewportController.h"
#include "LightManager.h"
#include "LightLinking.h"
#include "LightRigLibrary.h"

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
        ".pk3 .mdc .md5 .txt .smd .vta .m3 .3d .b3d .q3d .q3s .nff .nff .off .raw .ter .mdl .hmp .ndo .fbx .glb .gltf .vrm .tmd .rsd";

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
        LightManager::kill();
        SelectionSet::kill();
        delete m_pSingleton;
        m_pSingleton = nullptr;
    }
}

QString Manager::defaultImportExtensions()
{
    return mValidFileExtention;
}

////////////////////////////////////////
// Constructor & Destructor

Manager::Manager(MainWindow* parent):
    mRoot(nullptr), mSceneMgr(nullptr), mPlane(nullptr), m_pMainWindow(parent), m_pViewportGrid(nullptr)
{
    initRoot();         // Init Ogre Root
    initRenderSystem(); // Init Ogre Render System
    initSceneMgr();     // Init Ogre SceneManager

    HDREnvironmentManager::getSingleton();
    HdrViewportController::getSingleton();
    LightManager::getSingleton()->tryConnectToManager();

    if (SelectionSet *sel = SelectionSet::getSingletonPtr())
        sel->tryConnectToManager();
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

    LightRigLibrary::applyDefaultSceneLighting();

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
    LightLinking::onEntityCreated(ent);
    if(!mInitializingScene)
        SelectionSet::getSingleton()->selectOne(sceneNode);
    return ent;
}

Ogre::SceneNode* Manager::duplicateSceneNode(Ogre::SceneNode* source)
{
    if (!source || !mSceneMgr) return nullptr;

    // Generate a unique name based on the source
    QString baseName = QString::fromStdString(source->getName()) + "_copy";
    Ogre::SceneNode* newNode = addSceneNode(baseName);

    // Copy transform
    newNode->setPosition(source->getPosition());
    newNode->setOrientation(source->getOrientation());
    newNode->setScale(source->getScale());

    // Copy user object bindings (preserves primitive type, custom data, etc.)
    const auto& srcBindings = source->getUserObjectBindings();
    auto& dstBindings = newNode->getUserObjectBindings();
    // Copy the "default" user any (used by PrimitiveObject::isPrimitive)
    if (!srcBindings.getUserAny().isEmpty())
        dstBindings.setUserAny(srcBindings.getUserAny());

    // Deep-clone attached entities (each gets its own Mesh copy so
    // skeleton, animations, and materials are fully independent).
    for (unsigned short i = 0; i < source->numAttachedObjects(); ++i) {
        Ogre::MovableObject* obj = source->getAttachedObject(i);
        if (obj->getMovableType() != "Entity") continue;

        Ogre::Entity* srcEntity = static_cast<Ogre::Entity*>(obj);

        // Deep-clone mesh AND skeleton so animations are fully independent.
        // Mesh::clone() copies geometry but still references the same Skeleton resource.
        // We must also clone the skeleton and reassign it to the new mesh.
        // Use per-entity index to avoid name collisions when a node has multiple entities.
        QString cloneMeshName = QString::fromStdString(newNode->getName()) + "_mesh" +
                                (i > 0 ? QString::number(i) : QString());

        // Remove stale resources from a previous undo cycle (redo re-creates them)
        if (Ogre::MeshManager::getSingleton().getByName(cloneMeshName.toStdString()))
            Ogre::MeshManager::getSingleton().remove(cloneMeshName.toStdString());

        Ogre::MeshPtr clonedMesh = srcEntity->getMesh()->clone(cloneMeshName.toStdString());

        if (srcEntity->getMesh()->hasSkeleton()) {
            try {
                QString cloneSkelName = QString::fromStdString(newNode->getName()) + "_skel" +
                                       (i > 0 ? QString::number(i) : QString());

                // Remove stale skeleton from a previous undo cycle
                if (Ogre::SkeletonManager::getSingleton().getByName(cloneSkelName.toStdString()))
                    Ogre::SkeletonManager::getSingleton().remove(cloneSkelName.toStdString());

                Ogre::SkeletonPtr srcSkel = srcEntity->getMesh()->getSkeleton();

                // isManual=true tells Ogre not to try loading from disk
                Ogre::SkeletonPtr clonedSkel = Ogre::SkeletonManager::getSingleton().create(
                    cloneSkelName.toStdString(), srcSkel->getGroup(), true);

                // Copy bones using the skeleton's bone iterator (handles may not be sequential)
                auto srcBoneIt = srcSkel->getBoneIterator();
                while (srcBoneIt.hasMoreElements()) {
                    Ogre::Bone* srcBone = srcBoneIt.getNext();
                    Ogre::Bone* newBone = clonedSkel->createBone(
                        srcBone->getName(), srcBone->getHandle());
                    newBone->setPosition(srcBone->getInitialPosition());
                    newBone->setOrientation(srcBone->getInitialOrientation());
                    newBone->setScale(srcBone->getInitialScale());
                    newBone->setInitialState();
                }

                // Rebuild bone hierarchy
                auto srcBoneIt2 = srcSkel->getBoneIterator();
                while (srcBoneIt2.hasMoreElements()) {
                    Ogre::Bone* srcBone = srcBoneIt2.getNext();
                    Ogre::Node* srcParent = srcBone->getParent();
                    if (srcParent && srcSkel->hasBone(srcParent->getName())) {
                        Ogre::Bone* dstParent = clonedSkel->getBone(srcParent->getName());
                        Ogre::Bone* dstBone = clonedSkel->getBone(srcBone->getName());
                        if (dstParent && dstBone && !dstBone->getParent())
                            dstParent->addChild(dstBone);
                    }
                }
                clonedSkel->setBindingPose();

                // Copy animations
                for (unsigned short a = 0; a < srcSkel->getNumAnimations(); ++a) {
                    Ogre::Animation* srcAnim = srcSkel->getAnimation(a);
                    Ogre::Animation* dstAnim = clonedSkel->createAnimation(
                        srcAnim->getName(), srcAnim->getLength());
                    dstAnim->setInterpolationMode(srcAnim->getInterpolationMode());
                    dstAnim->setRotationInterpolationMode(srcAnim->getRotationInterpolationMode());

                    for (const auto& [handle, srcTrack] : srcAnim->_getNodeTrackList()) {
                        auto* dstTrack = dstAnim->createNodeTrack(handle);
                        try {
                            if (srcTrack->getAssociatedNode()) {
                                Ogre::Bone* dstBone = clonedSkel->getBone(handle);
                                if (dstBone) dstTrack->setAssociatedNode(dstBone);
                            }
                        } catch (...) { /* handle not found — skip association */ }
                        for (unsigned short k = 0; k < srcTrack->getNumKeyFrames(); ++k) {
                            const auto* kf = srcTrack->getNodeKeyFrame(k);
                            auto* dstKf = dstTrack->createNodeKeyFrame(kf->getTime());
                            dstKf->setTranslate(kf->getTranslate());
                            dstKf->setRotation(kf->getRotation());
                            dstKf->setScale(kf->getScale());
                        }
                    }
                }

                // Signal that the skeleton is fully loaded (populated in-memory)
                clonedSkel->_fireLoadingComplete();

                clonedMesh->setSkeletonName(clonedSkel->getName());
                clonedMesh->_notifySkeleton(clonedSkel);
            } catch (Ogre::Exception& e) {
                qWarning("Failed to clone skeleton: %s", e.getFullDescription().c_str());
            }
        }

        Ogre::Entity* newEntity = createEntity(newNode, clonedMesh);

        // Copy per-sub-entity material assignments
        for (unsigned int s = 0; s < srcEntity->getNumSubEntities(); ++s) {
            newEntity->getSubEntity(s)->setMaterial(
                srcEntity->getSubEntity(s)->getMaterial());
        }
    }

    return newNode;
}

Ogre::SceneNode* Manager::groupNodes(const QList<Ogre::SceneNode*>& nodes)
{
    if (nodes.isEmpty() || !mSceneMgr) return nullptr;

    SentryReporter::addBreadcrumb("ui.action",
        QString("Group %1 nodes").arg(nodes.size()));

    // Compute centroid of selected nodes (world positions)
    Ogre::Vector3 centroid = Ogre::Vector3::ZERO;
    for (Ogre::SceneNode* node : nodes)
        centroid += node->_getDerivedPosition();
    centroid /= static_cast<Ogre::Real>(nodes.size());

    // Find a common parent — use the parent of the first node
    Ogre::SceneNode* commonParent = static_cast<Ogre::SceneNode*>(nodes.first()->getParent());
    if (!commonParent)
        commonParent = mSceneMgr->getRootSceneNode();

    // Create the group node under the common parent
    QString baseName = "Group";
    unsigned int number = 0;
    while (mSceneMgr->hasSceneNode(QString(baseName + (number ? QString::number(number) : "")).toStdString()))
        ++number;

    QString groupName = baseName + (number ? QString::number(number) : "");
    Ogre::SceneNode* groupNode = commonParent->createChildSceneNode(groupName.toStdString());

    // Position the group at the centroid (in parent space)
    Ogre::Vector3 groupWorldPos = centroid;
    // Convert world position to local position relative to commonParent
    Ogre::Vector3 groupLocalPos = groupWorldPos;
    if (commonParent != mSceneMgr->getRootSceneNode()) {
        Ogre::Vector3 parentWorldPos = commonParent->_getDerivedPosition();
        Ogre::Quaternion parentWorldOrient = commonParent->_getDerivedOrientation();
        Ogre::Vector3 parentWorldScale = commonParent->_getDerivedScale();
        groupLocalPos = parentWorldOrient.Inverse() * ((groupWorldPos - parentWorldPos) / parentWorldScale);
    }
    groupNode->setPosition(groupLocalPos);

    // Reparent each selected node under the group, preserving world transform
    for (Ogre::SceneNode* node : nodes) {
        // Save the world transform
        Ogre::Vector3 worldPos = node->_getDerivedPosition();
        Ogre::Quaternion worldOrient = node->_getDerivedOrientation();
        Ogre::Vector3 worldScale = node->_getDerivedScale();

        // Remove from old parent
        Ogre::SceneNode* oldParent = static_cast<Ogre::SceneNode*>(node->getParent());
        if (oldParent)
            oldParent->removeChild(node);

        // Add to group
        groupNode->addChild(node);

        // Restore world transform by computing new local transform
        Ogre::Quaternion groupWorldOrient = groupNode->_getDerivedOrientation();
        Ogre::Vector3 groupWorldScale = groupNode->_getDerivedScale();
        Ogre::Vector3 groupDerivedPos = groupNode->_getDerivedPosition();

        node->setOrientation(groupWorldOrient.Inverse() * worldOrient);
        node->setScale(worldScale / groupWorldScale);
        node->setPosition(groupWorldOrient.Inverse() *
            ((worldPos - groupDerivedPos) / groupWorldScale));
    }

    emit sceneNodeCreated(groupNode);
    SelectionSet::getSingleton()->selectOne(groupNode);
    return groupNode;
}

void Manager::ungroupNode(Ogre::SceneNode* groupNode)
{
    if (!groupNode || !mSceneMgr) return;

    SentryReporter::addBreadcrumb("ui.action", "Ungroup node");

    Ogre::SceneNode* parentNode = static_cast<Ogre::SceneNode*>(groupNode->getParent());
    if (!parentNode)
        parentNode = mSceneMgr->getRootSceneNode();

    // Collect children (cannot modify during iteration)
    QList<Ogre::SceneNode*> children;
    for (auto& child : groupNode->getChildren()) {
        Ogre::SceneNode* childNode = static_cast<Ogre::SceneNode*>(child);
        if (!isForbiddenNodeName(QString::fromStdString(childNode->getName())))
            children.append(childNode);
    }

    // Reparent children to the group's parent, preserving world transforms
    for (Ogre::SceneNode* child : children) {
        Ogre::Vector3 worldPos = child->_getDerivedPosition();
        Ogre::Quaternion worldOrient = child->_getDerivedOrientation();
        Ogre::Vector3 worldScale = child->_getDerivedScale();

        groupNode->removeChild(child);
        parentNode->addChild(child);

        // Compute local transform relative to the new parent
        Ogre::Quaternion parentWorldOrient = parentNode->_getDerivedOrientation();
        Ogre::Vector3 parentWorldScale = parentNode->_getDerivedScale();
        Ogre::Vector3 parentDerivedPos = parentNode->_getDerivedPosition();

        child->setOrientation(parentWorldOrient.Inverse() * worldOrient);
        child->setScale(worldScale / parentWorldScale);
        child->setPosition(parentWorldOrient.Inverse() *
            ((worldPos - parentDerivedPos) / parentWorldScale));
    }

    // Destroy the now-empty group node
    emit sceneNodeDestroyed(groupNode);
    destroyAllAttachedMovableObjects(groupNode);
    mSceneMgr->destroySceneNode(groupNode);

    // Select the ungrouped children
    if (!children.isEmpty()) {
        SelectionSet::getSingleton()->selectOne(children.first());
        for (int i = 1; i < children.size(); ++i)
            SelectionSet::getSingleton()->append(children[i]);
    }
}

bool Manager::isGroupNode(Ogre::SceneNode* node) const
{
    if (!node) return false;
    // A group node has no attached objects (entities) and has children
    return node->numAttachedObjects() == 0 && node->numChildren() > 0;
}

bool Manager::isDescendantOf(Ogre::SceneNode* candidate, Ogre::SceneNode* ancestor)
{
    if (!candidate || !ancestor) return false;
    Ogre::Node* current = candidate->getParent();
    while (current) {
        if (current == ancestor) return true;
        current = current->getParent();
    }
    return false;
}

bool Manager::reparentNode(Ogre::SceneNode* node, Ogre::SceneNode* newParent)
{
    if (!node || !newParent || !mSceneMgr) return false;

    // Prevent reparenting to self
    if (node == newParent) return false;

    // Prevent cycles: newParent must not be a descendant of node
    if (isDescendantOf(newParent, node)) return false;

    // Already a child of newParent — nothing to do
    if (node->getParent() == newParent) return false;

    SentryReporter::addBreadcrumb("scene",
        QString("Reparent '%1' under '%2'")
            .arg(QString::fromStdString(node->getName()))
            .arg(QString::fromStdString(newParent->getName())));

    // Save world transform before reparenting
    node->_update(true, true);
    Ogre::Vector3 worldPos = node->_getDerivedPosition();
    Ogre::Quaternion worldOrient = node->_getDerivedOrientation();
    Ogre::Vector3 worldScale = node->_getDerivedScale();

    // Reparent
    Ogre::SceneNode* oldParent = static_cast<Ogre::SceneNode*>(node->getParent());
    if (oldParent)
        oldParent->removeChild(node);
    newParent->addChild(node);

    // Restore world transform as new local transform relative to newParent
    newParent->_update(true, true);
    Ogre::Quaternion parentWorldOrient = newParent->_getDerivedOrientation();
    Ogre::Vector3 parentWorldScale = newParent->_getDerivedScale();
    Ogre::Vector3 parentDerivedPos = newParent->_getDerivedPosition();

    node->setOrientation(parentWorldOrient.Inverse() * worldOrient);
    node->setScale(worldScale / parentWorldScale);
    node->setPosition(parentWorldOrient.Inverse() *
        ((worldPos - parentDerivedPos) / parentWorldScale));

    // Cascade-delete empty group ancestors: if removing this child left
    // the old parent empty, destroy it — then check ITS parent, and so on.
    Ogre::SceneNode* emptyCheck = oldParent;
    while (emptyCheck && emptyCheck != mSceneMgr->getRootSceneNode()
           && emptyCheck->numAttachedObjects() == 0
           && emptyCheck->numChildren() == 0) {
        Ogre::SceneNode* nextParent = static_cast<Ogre::SceneNode*>(emptyCheck->getParent());
        emit sceneNodeDestroyed(emptyCheck);
        destroyAllAttachedMovableObjects(emptyCheck);
        mSceneMgr->destroySceneNode(emptyCheck);
        emptyCheck = nextParent;
    }

    // Trigger scene tree rebuild
    emit sceneNodeCreated(node);

    return true;
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
void Manager::destroyAllUserRootNodes()
{
    if (!mSceneMgr)
        return;

    SentryReporter::addBreadcrumb("scene", "Destroy all user root scene nodes");

    // Rig-group lights are child scene nodes. destroySceneNode(name) uses
    // removeAndDestroyAllChildren() by default, which tears down Ogre light nodes
    // without unregistering them from LightManager — dangling handles → SIGSEGV.
    emit sceneClearing();
    if (auto* lights = LightManager::getSingletonPtr())
        lights->deleteAllUserLights();

    Ogre::SceneNode* root = mSceneMgr->getRootSceneNode();
    QStringList names;
    for (const auto& child : root->getChildren())
    {
        auto* childNode = static_cast<Ogre::SceneNode*>(child);
        const QString name = QString::fromStdString(childNode->getName());
        if (name.isEmpty() || isForbiddenNodeName(name))
            continue;
        // Match SceneTreeModel::buildChildren — skip transient gizmo rigs.
        if (name == QStringLiteral("BevelGizmo_Node")
            || name == QStringLiteral("BevelGizmo_Shaft")
            || name == QStringLiteral("BevelGizmo_Handle"))
            continue;
        names.append(name);
    }

    for (const QString& name : names)
        destroySceneNode(name);
}

void Manager::destroySceneNode(Ogre::SceneNode* node, bool destroyChildrenFirst)
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

    if (destroyChildrenFirst)
    {
        // Destroy children first so sceneNodeDestroyed fires per child (e.g. rig-group
        // lights unregister from LightManager before their Ogre objects are torn down).
        std::vector<Ogre::SceneNode*> childNodes;
        try
        {
            for (auto* child : node->getChildren())
                childNodes.push_back(static_cast<Ogre::SceneNode*>(child));
        }
        catch (...)
        {
        }
        for (Ogre::SceneNode* child : childNodes)
        {
            if (auto* lights = LightManager::getSingletonPtr())
            {
                if (lights->deleteLightBySceneNode(child))
                    continue;
            }
            destroySceneNode(child, true);
        }
    }
    else
    {
        try
        {
            node->removeAndDestroyAllChildren();
        }
        catch (...)
        {
        }
    }

    emit sceneNodeDestroyed(node);  // emitted before destruction so listeners can clean up while entities are still valid
    destroyAllAttachedMovableObjects(node);

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
    try {
        return mSceneMgr->hasSceneNode(_name.toStdString());
    } catch (...) {
        return false;
    }
}

static void collectSceneNodesRecursive(Ogre::SceneNode* parent, QList<Ogre::SceneNode*>& out)
{
    for (Ogre::Node* child : parent->getChildren())
    {
        Ogre::SceneNode* pSN = static_cast<Ogre::SceneNode*>(child);
        QString name = pSN->getName().data();
        if (!Manager::getSingletonPtr()->isForbiddenNodeName(name))
        {
            out.append(pSN);
            // Recurse into children (groups)
            if (pSN->numChildren() > 0)
                collectSceneNodesRecursive(pSN, out);
        }
    }
}

QList<Ogre::SceneNode *> &Manager::getSceneNodes()
{
    mSceneNodesList.clear();
    collectSceneNodesRecursive(getSceneMgr()->getRootSceneNode(), mSceneNodesList);
    return mSceneNodesList;
}

static void collectEntitiesRecursive(Ogre::SceneNode* parent, QList<Ogre::Entity*>& out)
{
    for (Ogre::Node* child : parent->getChildren())
    {
        Ogre::SceneNode* pSN = static_cast<Ogre::SceneNode*>(child);
        QString name = pSN->getName().data();
        if (!Manager::getSingletonPtr()->isForbiddenNodeName(name))
        {
            for (int entIndex = 0; entIndex < static_cast<int>(pSN->numAttachedObjects()); entIndex++)
            {
                Ogre::MovableObject* obj = pSN->getAttachedObject(entIndex);
                if (obj->getMovableType() == "Entity")
                    out.append(static_cast<Ogre::Entity*>(obj));
            }
            // Recurse into children (groups)
            if (pSN->numChildren() > 0)
                collectEntitiesRecursive(pSN, out);
        }
    }
}

QList<Ogre::Entity *> &Manager::getEntities()
{
    mEntitiesList.clear();
    collectEntitiesRecursive(getSceneMgr()->getRootSceneNode(), mEntitiesList);
    return mEntitiesList;
}

bool Manager::isForbiddenNodeName(const QString &_name)
{
    return (_name.isEmpty() //Ogre 14 creates unnamed nodes with empty string (e.g. SpaceCamera nodes)
            ||_name=="TPCameraChildSceneNode" //TODO add a define for TPCameraChildSceneNode
            ||_name=="GridLine_node" //TODO add a define for GridLine_node
            ||_name=="QtMeshDepthCameraNode" //offscreen depth-render camera (MeshDepthRenderer)
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
            QString archPath = QString::fromStdString(archName);
            if (QDir::isAbsolutePath(archPath)) {
                Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
                    archPath.toStdString(), typeName, secName);
                continue;
            }

            // Resolve relative paths against the application directory so that
            // resources are found regardless of the current working directory
            // (installed .deb, .app bundle, dev build). Build candidate roots
            // and pick the first that actually contains the path.
            //
            // On macOS the media/cfg tree lives under Contents/MacOS/ (==
            // applicationDirPath()), NOT at the .app bundle root. The previous
            // code resolved relative paths against macBundlePath() (the bundle
            // root), so in an installed .app every relative resource location —
            // including the RTSS GLSL programs and material textures — pointed
            // at a non-existent <App>.app/media/... directory. Ogre then loaded
            // no shaders/textures and every mesh rendered flat WHITE. Resolving
            // against applicationDirPath() first fixes it; the bundle-root path
            // stays as a fallback for any older layout. (#bug: white models in
            // Homebrew/installed builds, all platforms.)
            QStringList roots;
            roots << file;                                   // applicationDirPath()
#if OGRE_PLATFORM == OGRE_PLATFORM_APPLE
            roots << QString::fromStdString(macBundlePath()); // .app bundle root (legacy)
#endif
            QString resolved;
            for (const QString& root : roots) {
                const QString cand = root + "/" + archPath;
                if (QFileInfo::exists(cand)) { resolved = cand; break; }
            }
            // If none exist (e.g. an optional location), fall back to the first
            // candidate so Ogre logs a clear "resource location not found" for it
            // rather than silently skipping.
            if (resolved.isEmpty())
                resolved = roots.first() + "/" + archPath;

            Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
                resolved.toStdString(), typeName, secName);
        }
    }

    HdrBundledLibrary::registerUserHdriResourceLocation();

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
