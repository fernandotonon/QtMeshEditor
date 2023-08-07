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

#include "OgreXML/OgreXMLMeshSerializer.h"
#include "OgreXML/OgreXMLSkeletonSerializer.h"

#include "Manager.h"
#include "Assimp/Importer.h"

#ifndef WIN32
    #include <unistd.h>
#endif

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
                AssimpToOgreImporter importer;
                Ogre::MeshPtr mesh = importer.loadModel(file.filePath().toStdString());
                if (mesh)
                {
                    auto meshName = file.baseName();
                    sn = Manager::getSingleton()->addSceneNode(QString(meshName));

                    Ogre::Entity *en = Manager::getSingleton()->createEntity(sn, mesh);

                    sn->setPosition(0,0,0);

                    configureCamera(en);
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
                    // TODO: change the name of the skeleton to match the new mesh name.
                    Ogre::SkeletonSerializer ss;
                    ss.exportSkeleton(e->getSkeleton(),QString(file.path()+"/"+e->getMesh().get()->getSkeletonName().c_str()).toStdString().data());
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
