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
#include "MeshImporterExporter.h"
#include <QMessageBox>
#include <QFileInfo>
#include <QFileDialog>
#include <QDebug>

#include "Manager.h"
#include "Assimp/OgreAssimpLoader.h"
#include "OgreXML/OgreXMLMeshSerializer.h"
#include "OgreXML/OgreXMLSkeletonSerializer.h"

#ifndef WIN32
    #include <unistd.h>
#endif

MeshImporterExporter::MeshImporterExporter()
{
}

void MeshImporterExporter::configureCamera(Ogre::Entity *en)
{
    Ogre::Real size = std::max(std::max(en->getBoundingBox().getSize().y,en->getBoundingBox().getSize().x),en->getBoundingBox().getSize().z)    ;
    auto cameras = Manager::getSingleton()->getSceneMgr()->getCameras();
    for(const auto &tuple : cameras)
    {
        Ogre::Camera* camera = tuple.second;
        const Ogre::Radian fov = camera->getFOVy();
        Ogre::Real distance = size/(2*std::tan(fov.valueRadians()/2));
        camera->getParentSceneNode()->setPosition(0,0,-distance);
    }
}

void MeshImporterExporter::importer(const QStringList &_uriList)
{
    foreach(const QString &fileName,_uriList)
    {
        if(fileName.size())
        {
            QFileInfo file;
            file.setFile(fileName);

            Ogre::ResourceGroupManager::getSingleton().addResourceLocation(file.path().toStdString().data(),"FileSystem",file.path().toStdString().data(),false);
            Ogre::ResourceGroupManager::getSingleton().initialiseAllResourceGroups();

            Ogre::SceneNode *sn;

            if(!file.suffix().compare("mesh",Qt::CaseInsensitive))
            {
                sn = Manager::getSingleton()->addSceneNode(QString(file.baseName()));
                Ogre::Entity *en = Manager::getSingleton()->createEntity(sn, Ogre::MeshManager::getSingleton().load(file.fileName().toStdString().data(),                                                                                        file.path().toStdString().data()));
                sn->setPosition(0,0,0);

                configureCamera(en);
            }
            else if(!file.suffix().compare("xml",Qt::CaseInsensitive))
            {
                Ogre::MeshPtr mesh;
                sn = Manager::getSingleton()->addSceneNode(QString(file.baseName()));

                Ogre::Mesh* importedMesh = Ogre::MeshManager::getSingleton().createManual(sn->getName(), "General").get();
                Ogre::Skeleton* importedSk = Ogre::static_pointer_cast<Ogre::Skeleton>(Ogre::SkeletonManager::getSingleton().create(QString(QString(sn->getName().data())+".skeleton.xml").toStdString().data(), "General")).get();

                try
                {
                    Ogre::XMLSkeletonSerializer xmlSS;
                    xmlSS.importSkeleton((file.filePath().left(file.filePath().length()-8)+"skeleton.xml").toStdString().data(),importedSk);
                    importedSk->setBackgroundLoaded(true);
                    while(importedSk->isLoading())
                        #ifdef WIN32
                            Sleep(1000);
                        #else
                            sleep(1);
                        #endif

                    Ogre::XMLMeshSerializer xmlMS;
                    xmlMS.importMesh(file.filePath().toStdString().data(),importedMesh);
                    mesh = importedMesh->clone(sn->getName());

                }
                catch(...)
                {
                    Ogre::LogManager::getSingleton().logMessage("Trying with assimp...");
                    goto assimp;
                }
                Ogre::Entity *en = Manager::getSingleton()->createEntity(sn, mesh);
                sn->setPosition(0,0,0);

                configureCamera(en);
            }
            else
            {
                assimp:
                AssimpLoader::Options opts;
                opts.customAnimationName = "";
                opts.animationSpeedModifier = 1.0f;
                opts.postProcessSteps = 0x1|0x2|0x4|0x8|0x200|0x400|0x4000|0x1000000|0x8000000; // Assimp postprocess.h TODO: import it in the future for using a reliable enum
                opts.maxEdgeAngle = 30;
                opts.params = AssimpLoader::LP_CUT_ANIMATION_WHERE_NO_FURTHER_CHANGE;//|Ogre::AssimpLoader::LP_QUIET_MODE;

                // Get a unique name
                auto meshName = file.baseName();
                int i = 0;
                while(Ogre::MeshManager::getSingleton().getByName(meshName.toStdString())){
                    meshName = file.baseName() + " (" + std::to_string(++i).data() + ")";
                }
                Ogre::MeshPtr mesh =  Ogre::MeshManager::getSingleton().createManual(meshName.toStdString(), "General");
                Ogre::SkeletonPtr skeleton;
                Ogre::String filePath = file.filePath().toStdString();

                AssimpLoader loader;
                try{
                    if(loader.load(filePath, mesh.get(), skeleton,opts))
                    {
                        if(skeleton.get())
                        {
                            mesh->_notifySkeleton(skeleton);
                            mesh->setSkeletonName(skeleton.get()->getName());
                            for (unsigned short i = 0; i < skeleton->getNumBones(); ++i) {
                               Ogre::Bone* bone = skeleton->getBone(i);
                               Ogre::Quaternion orientation = bone->getOrientation();
                               Ogre::LogManager::getSingleton().logMessage("Bone: "+ std::to_string(i) + " - " + std::to_string(skeleton->getNumBones())+ " Nome: " + bone->getName() + " Orientation: " + Ogre::StringConverter::toString(orientation) + " Position: " + Ogre::StringConverter::toString(bone->getPosition()));

                            }
                            auto anim = skeleton->getAnimation(0);
                            for(unsigned int i = 0; i<anim->getNumNodeTracks(); ++i){
                                auto track = anim->getNodeTrack(i);
                                auto node = track->getAssociatedNode();
                                Ogre::LogManager::getSingleton().logMessage("node animation: "+node->getName()+" has "+std::to_string(track->getNumKeyFrames()));
                                for(unsigned int j = 0; j< track->getNumKeyFrames(); ++j){
                                    auto keyframe = track->getNodeKeyFrame(j);
                                    Ogre::LogManager::getSingleton().logMessage(" keyframe: " + Ogre::StringConverter::toString(keyframe->getRotation()));

                                }
                            }
                        }
                        sn = Manager::getSingleton()->addSceneNode(QString(meshName));
                        mesh = mesh->clone("Mesh_"+sn->getName());
                        Ogre::Entity *en = Manager::getSingleton()->createEntity(sn, mesh);

                        sn->setPosition(0,0,0);

                        configureCamera(en);
                    }
                    else
                    {
                        QMessageBox::warning(NULL,"Error when importing 3d file","Not supported by assimp.",QMessageBox::Ok);
                    }
                } catch (...){
                    QMessageBox::warning(NULL,"Error when importing 3d file","Not supported by assimp.",QMessageBox::Ok);
                }
            }
        }
    }
}

void MeshImporterExporter::exporter(Ogre::SceneNode *_sn)
{
    if(!_sn)
    {
        QMessageBox::warning(NULL,"No object","Which object are you trying to export?",QMessageBox::Ok);
        return;
    }

    QString filter;
    QString fileName = QFileDialog::getSaveFileName(NULL, QObject::tr("Export Mesh"),
                                                     _sn->getName().data(),
                                                     QObject::tr("Ogre Mesh (*.mesh);;"\
                                                        "Ogre Mesh v1.10+(*.mesh);;"\
                                                        "Ogre Mesh v1.8+(*.mesh);;"\
                                                        "Ogre Mesh v1.7+(*.mesh);;"\
                                                        "Ogre Mesh v1.4+(*.mesh);;"\
                                                        "Ogre Mesh v1.0+(*.mesh);;"\
                                                        "Ogre XML (*.mesh.xml)"),&filter,
                                                    QFileDialog::DontUseNativeDialog);
    if(fileName.size())
    {
        QFileInfo file;
        file.setFile(fileName);

        Ogre::Entity *e = Manager::getSingleton()->getSceneMgr()->getEntity(_sn->getName());
        if(e)
        {
            if(filter=="Ogre XML (*.mesh.xml)")
            {
                Ogre::XMLMeshSerializer xmlMS;
                Ogre::XMLSkeletonSerializer xmlSS;
                Ogre::MaterialSerializer ms;

                if(e->hasSkeleton())
                {
                    xmlSS.exportSkeleton(e->getSkeleton(),(fileName.left(fileName.length()-8)+"skeleton.xml").toStdString().data());
                }

                Ogre::String skName = e->getMesh().get()->getSkeletonName();

                e->getMesh().get()->setSkeletonName((file.baseName()+".skeleton.xml").toStdString().data());

                if(fileName.right(8)==".mesh.xml")
                {
                    fileName = fileName.left(fileName.length()-8);
                }
                fileName+=".mesh.xml";
                
                xmlMS.exportMesh(e->getMesh().get(),fileName.toStdString().data());

                e->getMesh().get()->setSkeletonName(skName);

                for(unsigned int c = 0; c<e->getNumSubEntities();c++)
                {
                    ms.queueForExport(e->getSubEntity(c)->getMaterial());
                }
                ms.exportQueued(QString(file.path()+"/"+file.baseName()+".material").toStdString().data());

            }
            else
            {
                Ogre::MeshSerializer m;
                Ogre::MaterialSerializer ms;

                unsigned int version = 0;
                if(filter=="Ogre Mesh (*.mesh)")version=0;
                if(filter=="Ogre Mesh v1.10+(*.mesh)")version=1;
                if(filter=="Ogre Mesh v1.8+(*.mesh)")version=2;
                if(filter=="Ogre Mesh v1.7+(*.mesh)")version=3;
                if(filter=="Ogre Mesh v1.4+(*.mesh)")version=4;
                if(filter=="Ogre Mesh v1.0+(*.mesh)")version=5;

                if(e->hasSkeleton())
                {
                    Ogre::SkeletonSerializer ss;
                    ss.exportSkeleton(e->getSkeleton(),QString(file.path()+"/"+file.baseName()+".skeleton").toStdString().data());

                    e->getMesh().get()->setSkeletonName(QString(file.baseName()+".skeleton").toStdString().data());
                }

                if(fileName.right(5)==".mesh")
                {
                    fileName = fileName.left(fileName.length()-5);
                }
                fileName+=".mesh";
                m.exportMesh(e->getMesh().get(),fileName.toStdString().data(),(Ogre::MeshVersion)version);
                
                for(unsigned int c = 0; c<e->getNumSubEntities();c++)
                {
                    ms.queueForExport(e->getSubEntity(c)->getMaterial());
                }
                ms.exportQueued(QString(file.path()+"/"+file.baseName()+".material").toStdString().data());
            }
        }
    }
}
