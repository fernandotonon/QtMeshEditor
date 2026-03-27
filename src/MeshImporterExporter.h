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

#ifndef MESHIMPORTEREXPORTER_H
#define MESHIMPORTEREXPORTER_H

#include <Ogre.h>
#include <QStringList>
#include <QFileInfo>
#include <functional>

#include "mainwindow.h"
#include "FBX/FBXExporter.h"

using ProgressCallback = std::function<void(int progress, const QString& status)>;

class MeshImporterExporter
{
private:
    static void configureCamera(const Ogre::Entity *en);
    static void exportMaterial(const Ogre::Entity *e, const QFileInfo &file);
    static void exportTextures(const Ogre::MaterialPtr &material, const QFileInfo &file);
    static const QMap<QString, QString> exportFormats;

public:
    static void configureCameraForTesting(const Ogre::Entity *en) { configureCamera(en); }
    static QString exportTextureName(const QString& originalName);
    static void importer(const QStringList &_uriList, unsigned int additionalFlags = 0);
    static QString exporter(const Ogre::SceneNode *_sn);
    static int exporter(const Ogre::SceneNode *_sn, const QString &_uri, const QString &_format);
    static QString formatFileURI(const QString &_uri, const QString &_format);
    static QString exportFileDialogFilter();

    static int sceneExporter(const QString &_uri, const ProgressCallback& progress = nullptr);
    static bool sceneImporter(const QString &_uri);
};

#endif // MESHIMPORTEREXPORTER_H
