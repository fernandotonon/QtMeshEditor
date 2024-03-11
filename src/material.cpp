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

#include "material.h"
#include "materialeditor.h"
#include "ui_material.h"
#include <QDebug>
#include <QFileDialog>
#include <OgreMaterialManager.h>
#include <OgreMaterialSerializer.h>
#include <OgreTechnique.h>

Material::Material(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::Material)
{
    ui->setupUi(this);
}

Material::~Material()
{
    delete ui;
}

void Material::UpdateMaterialList()
{
    QStringList List;
    Ogre::ResourceManager::ResourceMapIterator materialIterator = Ogre::MaterialManager::getSingleton().getResourceIterator();
    while (materialIterator.hasMoreElements())
    {
        List.append(Ogre::static_pointer_cast<Ogre::Material>(materialIterator.peekNextValue())->getName().data());
        materialIterator.moveNext();
    }
    SetMaterialList(List);
}

void Material::SetMaterialList(const QStringList &_list)
{
    ui->listMaterial->clear();
    ui->listMaterial->addItems(_list);
}

void Material::on_listMaterial_itemSelectionChanged()
{
    ui->buttonEdit->setEnabled(true);
    ui->buttonExport->setEnabled(true);
}

void Material::on_buttonEdit_clicked()
{
    MaterialEditor *ME = new MaterialEditor(this);
    ME->setMaterial(ui->listMaterial->selectedItems()[0]->text());
    ME->show();
}

void Material::on_buttonExport_clicked()
{
    QString fileName = QFileDialog::getSaveFileName(this, tr("Export material"),
                                                     "",
                                                     tr("Ogre Mesh (*.material)"),
                                                    nullptr, QFileDialog::DontUseNativeDialog);
    if(fileName.size())
    {

        Ogre::MaterialPtr material = Ogre::static_pointer_cast<Ogre::Material>(Ogre::MaterialManager::getSingleton().getByName(ui->listMaterial->selectedItems()[0]->text().toStdString().data()));
        Ogre::MaterialSerializer ms;
        ms.exportMaterial(material,fileName.toStdString().data());
    }
}

void Material::on_buttonNew_clicked()
{
    MaterialEditor *ME = new MaterialEditor(this);
    ME->setMaterial("");
    ME->show();
}

void Material::on_pushButton_clicked()
{
    QStringList filePaths = QFileDialog::getOpenFileNames(this, tr("Select the materials"),
                                                     "",
                                                     tr("Ogre Material (*.material)"),
                                                     nullptr, QFileDialog::DontUseNativeDialog);

    foreach(const QString &filePath, filePaths)
    {
        if(filePath.size())
        {
            QFileInfo file;
            file.setFile(filePath);

            Ogre::ResourceGroupManager::getSingleton().addResourceLocation(file.path().toStdString().data(),"FileSystem",file.filePath().toStdString().data(),true);
            Ogre::ResourceGroupManager::getSingleton().initialiseAllResourceGroups();

            Ogre::MaterialManager::getSingleton().reloadAll(true);
            Ogre::MeshManager::getSingleton().reloadAll(true);

            UpdateMaterialList();
        }
    }
}

void Material::on_listMaterial_itemDoubleClicked(QListWidgetItem *item)
{
    MaterialEditor *ME = new MaterialEditor(this);
    ME->setMaterial(item->text());
    ME->show();
}

