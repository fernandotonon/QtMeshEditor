/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) HogPog Team (www.hogpog.com.br)

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
#include "MeshImporterExporter.h"
#include <assimp/Importer.hpp>
#include <assimp/Exporter.hpp>
#include <assimp/postprocess.h>
#include <QMessageBox>
#include <QFileDialog>
#include <QDebug>

#include "OgreXML/OgreXMLMeshSerializer.h"
#include "OgreXML/OgreXMLSkeletonSerializer.h"

#include "Manager.h"
#include "Assimp/Importer.h"

#ifndef WIN32
    #include <unistd.h>
#endif

const QMap<QString, QString> MeshImporterExporter::exportFormats = {
    {"Ogre Mesh (*.mesh)", ".mesh"},
    {"Ogre Mesh v1.10+(*.mesh)", ".mesh"},
    {"Ogre Mesh v1.8+(*.mesh)", ".mesh"},
    {"Ogre Mesh v1.7+(*.mesh)", ".mesh"},
    {"Ogre Mesh v1.4+(*.mesh)", ".mesh"},
    {"Ogre Mesh v1.0+(*.mesh)", ".mesh"},
    {"Ogre XML (*.mesh.xml)", ".mesh.xml"},
    {"Collada (*.dae)", ".dae"},
    {"X (*.x)", ".x"},
    {"STP (*.stp)", ".stp"},
    {"OBJ (*.obj)", ".obj"},
    {"OBJ without MTL (*.objnomtl)", ".obj"},
    {"STL (*.stl)", ".stl"},
    {"STL Binary (*.stlb)", ".stlb"},
    {"PLY (*.ply)", ".ply"},
    {"PLY Binary (*.plyb)", ".plyb"},
    {"3DS (*.3ds)", ".3ds"},
    {"glTF 2.0 (*.gltf2)", ".gltf2"},
    {"glTF 2.0 Binary (*.glb2)", ".glb2"},
    {"glTF 1.0 (*.gltf)", ".gltf"},
    {"glTF 1.0 Binary (*.glb)", ".glb"},
    {"Assimp Binary (*.assbin)", ".assbin"}
};

void MeshImporterExporter::configureCamera(const Ogre::Entity *en)
{
    Ogre::Real size = std::max(std::max(en->getBoundingBox().getSize().y,en->getBoundingBox().getSize().x),en->getBoundingBox().getSize().z)    ;
    auto cameras = Manager::getSingleton()->getSceneMgr()->getCameras();
    for(const auto &[_, camera] : cameras)
    {
        const Ogre::Radian fov = camera->getFOVy();
        Ogre::Real distance = size/(2*std::tan(fov.valueRadians()/2));
        camera->getParentSceneNode()->setPosition(0,0,-distance);
    }
}

void MeshImporterExporter::exportMaterial(const Ogre::Entity* e, const QFileInfo& file)
{
    Ogre::MaterialSerializer ms;
    for (const auto &subEntity : e->getSubEntities())
    {
        ms.queueForExport(subEntity->getMaterial());
        exportTextures(subEntity->getMaterial(), file);
    }
    ms.exportQueued((file.path() + "/" + file.baseName() + ".material").toStdString());
}

void MeshImporterExporter::exportTextures(const Ogre::MaterialPtr& material, const QFileInfo& file)
{
    for (const auto &technique : material->getTechniques())
    {
        for (const auto &pass : technique->getPasses())
        {
            for (const auto &tus : pass->getTextureUnitStates())
            {
                if (tus->getContentType() != Ogre::TextureUnitState::CONTENT_NAMED)
                    continue;

                Ogre::TexturePtr tex = tus->_getTexturePtr();
                if (tex->getTextureType() == Ogre::TEX_TYPE_2D)
                {
                    Ogre::Image img;
                    tex->convertToImage(img, true);
                    img.save((file.path() + "/" + tex->getName().c_str()).toStdString());
                }
            }
        }
    }
}

void MeshImporterExporter::importer(const QStringList &_uriList)
{
    try{
        foreach(const QString &fileName,_uriList)
        {
            if(!fileName.size()) continue;

            QFileInfo file;
            file.setFile(fileName);

            if(!Ogre::ResourceGroupManager::getSingleton().resourceLocationExists(file.path().toStdString().data(),file.path().toStdString().data()))
            {
                Ogre::ResourceGroupManager::getSingleton().addResourceLocation(file.path().toStdString().data(),"FileSystem",file.path().toStdString().data());
                Ogre::ResourceGroupManager::getSingleton().initialiseAllResourceGroups();
            }

            Ogre::SceneNode *sn;
            const Ogre::Entity *en;

            if(!file.suffix().compare("mesh",Qt::CaseInsensitive))
            {
                sn = Manager::getSingleton()->addSceneNode(QString(file.baseName()));
                en = Manager::getSingleton()->createEntity(sn, Ogre::MeshManager::getSingleton().load(file.fileName().toStdString().data(), file.path().toStdString().data()));
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
                en = Manager::getSingleton()->createEntity(sn, mesh);
            }
            else
            {
                assimp:
                AssimpToOgreImporter importer;
                Ogre::MeshPtr mesh = importer.loadModel(file.filePath().toStdString());
                if (!mesh) return;

                auto meshName = file.baseName();
                sn = Manager::getSingleton()->addSceneNode(QString(meshName));
                en = Manager::getSingleton()->createEntity(sn, mesh);
            }

            sn->setPosition(0,0,0);
            configureCamera(en);
        }
    } 
    catch(Ogre::Exception &e)
    {
        Ogre::LogManager::getSingleton().logMessage(e.getFullDescription());
    }
}

QString MeshImporterExporter::formatFileURI(const QString &_uri, const QString &_format)
{
    const auto ext = exportFormats[_format];
    if(_uri.right(ext.size())==ext) 
        return _uri;
    
    return _uri+ext;
}

QString MeshImporterExporter::exportFileDialogFilter()
{
    QString filter;
    for(auto format = exportFormats.keyBegin(); format!=exportFormats.keyEnd(); ++format)
        filter+=*format+";;";
    filter.chop(2);
    return filter;
}

void MeshImporterExporter::exporter(const Ogre::SceneNode *_sn)
{
    if(!_sn)
    {
        QMessageBox::warning(nullptr,"No object","Which object are you trying to export?",QMessageBox::Ok);
        return;
    }

    QString filter = "Ogre Mesh (*.mesh)";
    QString fileName = QFileDialog::getSaveFileName(nullptr, QObject::tr("Export Mesh"),
                                                     _sn->getName().data(),
                                                     exportFileDialogFilter(),&filter,
                                                    QFileDialog::DontUseNativeDialog);
    if(!fileName.size()) return;

    fileName = formatFileURI(fileName, filter);

    QFileInfo file;
    file.setFile(fileName);

    const Ogre::Entity *e = Manager::getSingleton()->getSceneMgr()->getEntity(_sn->getName());
    if(!e) return;

    if(filter=="Ogre XML (*.mesh.xml)")
    {
        Ogre::XMLMeshSerializer xmlMS;
        Ogre::String skName;

        if(e->hasSkeleton())
        {
            Ogre::XMLSkeletonSerializer xmlSS;
            xmlSS.exportSkeleton(e->getSkeleton(),(fileName.left(fileName.length()-8)+"skeleton.xml").toStdString().data());

            skName = e->getMesh().get()->getSkeletonName();

            e->getMesh().get()->setSkeletonName((file.baseName()+".skeleton.xml").toStdString().data());
        }

        xmlMS.exportMesh(e->getMesh().get(),fileName.toStdString().data());

        if(e->hasSkeleton())
            e->getMesh().get()->setSkeletonName(skName);

        exportMaterial(e, file);
    }
    else if(filter.contains("mesh"))
    {
        Ogre::MeshSerializer m;

        unsigned int version = 0;
        std::map<QString, unsigned int> versionMap = {
            {"Ogre Mesh (*.mesh)", 0},
            {"Ogre Mesh v1.10+(*.mesh)", 1},
            {"Ogre Mesh v1.8+(*.mesh)", 2},
            {"Ogre Mesh v1.7+(*.mesh)", 3},
            {"Ogre Mesh v1.4+(*.mesh)", 4},
            {"Ogre Mesh v1.0+(*.mesh)", 5}
        };

        version = versionMap[filter];

        if(e->hasSkeleton())
        {
            // TODO: change the name of the skeleton to match the new mesh name.
            Ogre::SkeletonSerializer ss;
            ss.exportSkeleton(e->getSkeleton(),QString(file.path()+"/"+e->getMesh().get()->getSkeletonName().c_str()).toStdString().data());
        }

        m.exportMesh(e->getMesh().get(),fileName.toStdString().data(),(Ogre::MeshVersion)version);

        exportMaterial(e, file);
    } else {
        // Export using Assimp

        // Export temp Ogre XML
        Ogre::XMLMeshSerializer m;
        /*Ogre::String skName;
        if(e->hasSkeleton())
        {
            skName = e->getMesh().get()->getSkeletonName();
            Ogre::XMLSkeletonSerializer xmlSS;
            xmlSS.exportSkeleton(e->getSkeleton(),skName);
        }*/
        m.exportMesh(e->getMesh().get(),"./temp.mesh.xml");
        QFileInfo temp = file;
        temp.setFile("./temp.material");
        exportMaterial(e, temp);

        // Import the temp file
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(
            "./temp.mesh.xml",
            aiProcess_CalcTangentSpace |
            aiProcess_JoinIdenticalVertices |
            aiProcess_Triangulate |
            aiProcess_RemoveComponent |
            aiProcess_GenSmoothNormals |
            aiProcess_ValidateDataStructure |
            aiProcess_OptimizeGraph |
            aiProcess_LimitBoneWeights |
            aiProcess_SortByPType |
            aiProcess_ImproveCacheLocality |
            aiProcess_FixInfacingNormals |
            aiProcess_PopulateArmatureData | // necessary to load bone node information
            aiProcess_OptimizeMeshes |
            aiProcess_GlobalScale
        );
        if(!scene) return;

        // Export to the desired format
        Assimp::Exporter exporter;
        exporter.Export(scene, file.suffix().toStdString().c_str(), file.filePath().toStdString().c_str());

        // Remove temporary files
        QFile::remove("./temp.mesh.xml");
       // QFile::remove("./temp.skeleton.xml");
        QFile::remove("./temp.material");
    }
}
