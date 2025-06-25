#include "material.h"
#include "MaterialEditorQML.h"
#include "ui_material.h"
#include <QDebug>
#include <QFileDialog>
#include <QMessageBox>
#include <QQmlApplicationEngine>
#include <QQuickWidget>
#include <OgreMaterialManager.h>
#include <OgreMaterialSerializer.h>
#include <OgreTechnique.h>
#include <QDir>
#include <QResource>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickRenderControl>
#include <QCoreApplication>
#include <QSGRendererInterface>

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
    try {
        // Force software rendering to avoid OpenGL conflicts with Ogre
        qputenv("QSG_RHI_BACKEND", "software");
        qputenv("QT_QUICK_BACKEND", "software");
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        
        // Load the selected material first
        MaterialEditorQML* qmlEditor = MaterialEditorQML::qmlInstance(nullptr, nullptr);
        qmlEditor->loadMaterial(ui->listMaterial->selectedItems()[0]->text());
        
        // Create QML Application Engine for standalone window
        QQmlApplicationEngine* engine = new QQmlApplicationEngine(this);
        
        // Force software rendering on the engine
        engine->setProperty("_q_sg_renderloop", "basic");
        
        // Register QML types if not already registered
        qmlRegisterSingletonType<MaterialEditorQML>("MaterialEditorQML", 1, 0, "MaterialEditorQML", 
            [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject * {
                Q_UNUSED(engine)
                Q_UNUSED(scriptEngine)
                return MaterialEditorQML::qmlInstance(engine, scriptEngine);
            });
        
        // Set window properties in QML context
        engine->rootContext()->setContextProperty("materialName", ui->listMaterial->selectedItems()[0]->text());
        
        // Load the QML material editor
        QUrl qmlUrl("qrc:/MaterialEditorQML/MaterialEditorWindow.qml");
        qDebug() << "Attempting to load QML from:" << qmlUrl.toString();
        
        // Connect to check for loading errors
        connect(engine, &QQmlApplicationEngine::objectCreated, this, [this, engine](QObject *obj, const QUrl &objUrl) {
            if (!obj) {
                qDebug() << "QML failed to load";
                engine->deleteLater();
                
                QMessageBox::critical(this, "QML Editor Error", 
                    "QML Material Editor failed to load. Please check the QML files and try again.");
                
            } else {
                qDebug() << "QML Material Editor loaded successfully";
                // Set window title
                if (auto window = qobject_cast<QQuickWindow*>(obj)) {
                    window->setTitle("QML Material Editor - " + ui->listMaterial->selectedItems()[0]->text());
                }
            }
        });
        
        engine->load(qmlUrl);
        
    } catch (const std::exception& e) {
        qDebug() << "Exception in QML creation:" << e.what();
        QMessageBox::critical(this, "Material Editor Error", 
            QString("QML Material Editor encountered an error: %1").arg(e.what()));
        
    } catch (...) {
        qDebug() << "Unknown exception in QML creation";
        QMessageBox::critical(this, "Material Editor Error", 
            "QML Material Editor encountered an unknown error.");
    }
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
    try {
        // Force software rendering to avoid OpenGL conflicts with Ogre
        qputenv("QSG_RHI_BACKEND", "software");
        qputenv("QT_QUICK_BACKEND", "software");
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        
        // Create a new material in the QML editor
        MaterialEditorQML* qmlEditor = MaterialEditorQML::qmlInstance(nullptr, nullptr);
        qmlEditor->createNewMaterial("NewMaterial");
        
        // Create QML Application Engine for standalone window
        QQmlApplicationEngine* engine = new QQmlApplicationEngine(this);
        
        // Force software rendering on the engine
        engine->setProperty("_q_sg_renderloop", "basic");
        
        // Register QML types if not already registered
        qmlRegisterSingletonType<MaterialEditorQML>("MaterialEditorQML", 1, 0, "MaterialEditorQML", 
            [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject * {
                Q_UNUSED(engine)
                Q_UNUSED(scriptEngine)
                return MaterialEditorQML::qmlInstance(engine, scriptEngine);
            });
        
        // Set window properties in QML context
        engine->rootContext()->setContextProperty("materialName", "NewMaterial");
        
        // Load the QML material editor
        QUrl qmlUrl("qrc:/MaterialEditorQML/MaterialEditorWindow.qml");
        
        // Connect to check for loading errors
        connect(engine, &QQmlApplicationEngine::objectCreated, this, [this, engine](QObject *obj, const QUrl &objUrl) {
            if (!obj) {
                qDebug() << "QML failed to load";
                engine->deleteLater();
                QMessageBox::critical(this, "QML Editor Error", 
                    "QML Material Editor failed to load. Please check the QML files and try again.");
            } else {
                qDebug() << "QML Material Editor loaded successfully for new material";
                // Set window title
                if (auto window = qobject_cast<QQuickWindow*>(obj)) {
                    window->setTitle("QML Material Editor - New Material");
                }
            }
        });
        
        engine->load(qmlUrl);
        
    } catch (const std::exception& e) {
        qDebug() << "Exception in QML creation:" << e.what();
        QMessageBox::critical(this, "Material Editor Error", 
            QString("QML Material Editor encountered an error: %1").arg(e.what()));
    } catch (...) {
        qDebug() << "Unknown exception in QML creation";
        QMessageBox::critical(this, "Material Editor Error", 
            "QML Material Editor encountered an unknown error.");
    }
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
    // Use the QML editor when double-clicking a material
    on_buttonEdit_clicked();
}

