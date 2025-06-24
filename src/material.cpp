#include "material.h"
#include "materialeditor.h"
#include "MaterialEditorQML.h"
#include "ui_material.h"
#include <QDebug>
#include <QFileDialog>
#include <QQmlApplicationEngine>
#include <QQuickWidget>
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

void Material::on_buttonEditQML_clicked()
{
    // Create QML Material Editor Window
    QQuickWidget* qmlWidget = new QQuickWidget(this);
    qmlWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    qmlWidget->setAttribute(Qt::WA_DeleteOnClose);
    qmlWidget->setWindowTitle("QML Material Editor - " + ui->listMaterial->selectedItems()[0]->text());
    qmlWidget->resize(1200, 800);
    
    // Load the QML material editor
    qmlWidget->setSource(QUrl("qrc:/qml/MaterialEditorWindow.qml"));
    
    // Load the selected material
    MaterialEditorQML* qmlEditor = MaterialEditorQML::qmlInstance(nullptr, nullptr);
    qmlEditor->loadMaterial(ui->listMaterial->selectedItems()[0]->text());
    
    qmlWidget->show();
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
    auto ME = new MaterialEditor(this);
    ME->setMaterial(item->text());
    ME->show();
}

