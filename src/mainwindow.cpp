#include <QMessageBox>
#include <QSettings>
#include <QApplication>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonArray>
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "OgreWidget.h"
#include "QtInputManager.h"
#include "Manager.h"
#include "material.h"
#include "about.h"
#include "PrimitivesWidget.h"
#include "MeshImporterExporter.h"
#include "EditorViewport.h"
#include "ViewportGrid.h"
#include "TransformWidget.h"
#include "MaterialWidget.h"
#include "AnimationWidget.h"
#include "SelectionSet.h"
#include "animationcontrolwidget.h"

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent), ui(new Ui::MainWindow),
    customPaletteColorDialog(new QColorDialog(this)),
    ambientLightColorDialog(new QColorDialog(this))
{
    ui->setupUi(this);

    // Essential behavior of mainWindow for docking widget
    setDockNestingEnabled(true);
    setDockOptions(dockOptions() & (~QMainWindow::AllowTabbedDocks));
    setCentralWidget(nullptr);  // Explicitly define that there is no central widget so dockable widget will take the place

    Manager* manager = Manager::getSingleton(this); // init the Ogre Root/RenderSystem/SceneManager

    createEditorViewport(/*TODO add the type of view (perspective, left,....*/);

    manager->loadResources(); // Resources should be loaded after createRenderWindow...

    m_pRoot = manager->getRoot();
    m_pRoot->addFrameListener(this);

    manager->CreateEmptyScene();

    initToolBar();

    QSettings settings;
    mCurrentPalette = settings.value("palette","dark").toString();
    if(mCurrentPalette == "light"){
            ui->actionLight->setChecked(true);
    } else if(mCurrentPalette == "custom"){
        custom_Palette_Color_Selected(settings.value("customPalette").value<QColor>());
        ui->actionCustom->blockSignals(true);
        ui->actionCustom->setChecked(true);
        ui->actionCustom->blockSignals(false);
    } else {
        ui->actionDark->setChecked(true);
    }

    customPaletteColorDialog->setOption(QColorDialog::DontUseNativeDialog);
    customPaletteColorDialog->setObjectName("Custom Color Dialog");
    QObject::connect(customPaletteColorDialog,&QColorDialog::colorSelected,this,[=](const QColor &color){
        custom_Palette_Color_Selected(color);
    });

    ambientLightColorDialog->setOption(QColorDialog::DontUseNativeDialog);
    ambientLightColorDialog->setObjectName("Ambient Light Color Dialog");
    QObject::connect(ambientLightColorDialog,&QColorDialog::colorSelected,this,[=](const QColor &color){
        if(color.isValid())
            Manager::getSingleton()->getSceneMgr()->setAmbientLight( Ogre::ColourValue(color.redF(),color.greenF(),color.blueF()) );
    });

    ///// Workaround, when using mRoot->startRendering() there's a flickering effect on the grid
    m_pTimer = new QTimer(this);
    connect(m_pTimer, &QTimer::timeout, this, [this](){
        m_pRoot->renderOneFrame();
    });
    m_pTimer->start(0);
}

/////////////////////////// TODO Clean up the code of MainWindow
/// /////////////////////// TODO improve the ui (toolbar, menubar,....) and add translation (obviously Portuguese but french, english, may be japaneese !)
MainWindow::~MainWindow()
{
    foreach (EditorViewport* pOgreWidget, mDockWidgetList)
    {
        pOgreWidget->close();
    }
    mDockWidgetList.clear();

    m_pRoot->removeFrameListener(this);

    if(m_pTimer)
    {
        m_pTimer->stop();
        delete m_pTimer;
        m_pTimer = nullptr;
    }

    delete ui;
    if(m_pTransformWidget)
    {
        delete m_pTransformWidget;
        m_pTransformWidget = nullptr;
    }
    if(m_pPrimitivesWidget)
    {
        delete m_pPrimitivesWidget;
        m_pPrimitivesWidget = nullptr;
    }
    if(m_pMaterialWidget)
    {
        delete m_pMaterialWidget;
        m_pMaterialWidget = nullptr;
    }
}

void MainWindow::initToolBar()
{
    //Import the mesh's sent by parameter
    QStringList uris = QCoreApplication::arguments();
    for(int c=uris.size()-1;c>=0;--c)
    {
        QString uri = uris.at(c);

        if(Manager::getSingleton()->isValidFileExtention(uri))
        {
            uri.replace("%20"," ");
            uris.replace(c,uri);
        }
        else
        {
            uris.removeAt(c);
        }
    }

    mUriList.append(uris);

    // Transform Property tab
    m_pTransformWidget = new TransformWidget(this->ui->tabWidget);
    ui->tabWidget->addTab(m_pTransformWidget, tr("Transform"));
    setTransformState(TransformOperator::TS_SELECT);
    connect(ui->actionSelect_Object, &QAction::triggered, this, [this]{setTransformState(TransformOperator::TS_SELECT);});
    connect(ui->actionTranslate_Object, &QAction::triggered, this, [this]{setTransformState(TransformOperator::TS_TRANSLATE);});
    connect(ui->actionRotate_Object, &QAction::triggered, this, [this]{setTransformState(TransformOperator::TS_ROTATE);});
    connect(ui->actionRemove_Object, SIGNAL(triggered()), TransformOperator::getSingleton(), SLOT(removeSelected()));

    // Material tab
    m_pMaterialWidget = new MaterialWidget(this->ui->tabWidget);
    ui->tabWidget->addTab(m_pMaterialWidget, tr("Material"));

    // Create Primitive Object Menu
    // & PrimitivesWidget
    m_pPrimitivesWidget = new PrimitivesWidget(this->ui->tabWidget);
    ui->tabWidget->addTab(m_pPrimitivesWidget, tr("Edit"));

    auto addPrimitiveButton = new QToolButton(ui->objectsToolbar);
    addPrimitiveButton->setIcon(QIcon(":/icones/cube.png"));
    addPrimitiveButton->setToolTip(tr("Add Primitive"));
    addPrimitiveButton->setPopupMode(QToolButton::InstantPopup);

    auto addPrimitiveMenu = new QMenu(addPrimitiveButton);

    auto pAddCube       = new QAction(QIcon(":/icones/cube.png"),tr("Cube"), addPrimitiveButton);
    auto pAddSphere     = new QAction(QIcon(":/icones/sphere.png"),tr("Sphere"), addPrimitiveButton);
    auto pAddPlane      = new QAction(QIcon(":/icones/square.png"),tr("Plane"), addPrimitiveButton);
    auto pAddCylinder   = new QAction(QIcon(":/icones/cylinder.png"),tr("Cylinder"), addPrimitiveButton);
    auto pAddCone       = new QAction(QIcon(":/icones/cone.png"),tr("Cone"), addPrimitiveButton);
    auto pAddTorus      = new QAction(QIcon(":/icones/torus.png"),tr("Torus"), addPrimitiveButton);
    // TODO add correct icon for tube and polish the existing ones
    auto pAddTube       = new QAction(QIcon(":/icones/torus.png"),tr("Tube"), addPrimitiveButton);
    auto pAddCapsule    = new QAction(QIcon(":/icones/capsule.png"),tr("Capsule"), addPrimitiveButton);
    auto pAddIcoSphere  = new QAction(QIcon(":/icones/sphere.png"),tr("IcoSphere"), addPrimitiveButton);
    auto pAddRoundedBox = new QAction(QIcon(":/icones/roundedbox.png"),tr("Rounded Box"), addPrimitiveButton);
    auto pAddSpring     = new QAction(QIcon(":/icones/spring.png"),tr("Spring"), addPrimitiveButton);

    addPrimitiveMenu->addAction(pAddCube);
    addPrimitiveMenu->addAction(pAddSphere);
    addPrimitiveMenu->addAction(pAddPlane);
    addPrimitiveMenu->addAction(pAddCylinder);
    addPrimitiveMenu->addAction(pAddCone);
    addPrimitiveMenu->addAction(pAddTorus);
    addPrimitiveMenu->addAction(pAddTube);
    addPrimitiveMenu->addAction(pAddCapsule);
    addPrimitiveMenu->addAction(pAddIcoSphere);
    addPrimitiveMenu->addAction(pAddRoundedBox);
    addPrimitiveMenu->addAction(pAddSpring);

    addPrimitiveButton->setMenu(addPrimitiveMenu);
    ui->objectsToolbar->addWidget(addPrimitiveButton);

    connect(pAddCube,       SIGNAL(triggered()),m_pPrimitivesWidget,SLOT(createCube()));
    connect(pAddSphere,     SIGNAL(triggered()),m_pPrimitivesWidget,SLOT(createSphere()));
    connect(pAddPlane,      SIGNAL(triggered()),m_pPrimitivesWidget,SLOT(createPlane()));
    connect(pAddCylinder,   SIGNAL(triggered()),m_pPrimitivesWidget,SLOT(createCylinder()));
    connect(pAddCone,       SIGNAL(triggered()),m_pPrimitivesWidget,SLOT(createCone()));
    connect(pAddTorus,      SIGNAL(triggered()),m_pPrimitivesWidget,SLOT(createTorus()));
    connect(pAddTube,       SIGNAL(triggered()),m_pPrimitivesWidget,SLOT(createTube()));
    connect(pAddCapsule,    SIGNAL(triggered()),m_pPrimitivesWidget,SLOT(createCapsule()));
    connect(pAddIcoSphere,  SIGNAL(triggered()),m_pPrimitivesWidget,SLOT(createIcoSphere()));
    connect(pAddRoundedBox, SIGNAL(triggered()),m_pPrimitivesWidget,SLOT(createRoundedBox()));
    connect(pAddSpring,     SIGNAL(triggered()),m_pPrimitivesWidget,SLOT(createSpring()));

    // Animation tab
    auto pAnimationWidget = new AnimationWidget(this->ui->tabWidget);
    ui->tabWidget->addTab(pAnimationWidget, tr("Animation"));

    // Add Animation Control Widget to the bottom of the main window
    auto pAnimationControlWidget = new AnimationControlWidget(this);
    pAnimationControlWidget->setVisible(false);
    connect(pAnimationWidget,SIGNAL(changeAnimationName(const std::string&)),pAnimationControlWidget,SLOT(updateAnimationTree()));

    //connect(m_pTransformWidget, SIGNAL(selectionChanged(QString)), pAnimationWidget, SLOT(updateAnimationTable()));
    connect(pAnimationWidget,SIGNAL(changeAnimationState(bool)),this,SLOT(setPlaying(bool)));
    connect(ui->tabWidget,&QTabWidget::currentChanged,this,[=](int index){
        if(index==3){
            // Add Animation Control Widget to the bottom of the main window
            pAnimationControlWidget->setVisible(true);
            addDockWidget(Qt::BottomDockWidgetArea, pAnimationControlWidget);
        } else {
            // Remove Animation Control Widget from the bottom of the main window
            auto dockWidget = findChild<QDockWidget*>("AnimationControlWidget");
            if (dockWidget) {
                pAnimationControlWidget->setVisible(false);
                removeDockWidget(dockWidget);
            }
        }
    });

    // Viewport
    connect(ui->actionAdd_Viewport, SIGNAL(triggered()), this, SLOT(createEditorViewport()));
    connect(ui->actionChange_BG_Color, SIGNAL(triggered()), this, SLOT(chooseBgColor()));

    // show grid
    connect(ui->actionShow_Grid, SIGNAL(toggled(bool)),Manager::getSingleton()->getViewportGrid(),SLOT(setVisible(bool)));
}

const QPalette &MainWindow::darkPalette()
{
    auto darkPalette = new QPalette();

    darkPalette->setColor(QPalette::Window,          QColor( 37,  37,  37));
    darkPalette->setColor(QPalette::WindowText,      QColor(212, 212, 212));
    darkPalette->setColor(QPalette::Base,            QColor( 60,  60,  60));
    darkPalette->setColor(QPalette::AlternateBase,   QColor( 45,  45,  45));
    darkPalette->setColor(QPalette::PlaceholderText, QColor(127, 127, 127));
    darkPalette->setColor(QPalette::Text,            QColor(212, 212, 212));
    darkPalette->setColor(QPalette::Link,            QColor(100, 100, 100));
    darkPalette->setColor(QPalette::Button,          QColor( 45,  45,  45));
    darkPalette->setColor(QPalette::ButtonText,      QColor(212, 212, 212));
    darkPalette->setColor(QPalette::BrightText,      QColor(240, 240, 240));
    darkPalette->setColor(QPalette::Highlight,       QColor( 38,  79, 120));
    darkPalette->setColor(QPalette::HighlightedText, QColor(240, 240, 240));

    darkPalette->setColor(QPalette::Light,           QColor( 60,  60,  60));
    darkPalette->setColor(QPalette::Midlight,        QColor( 52,  52,  52));
    darkPalette->setColor(QPalette::Dark,            QColor( 30,  30,  30) );
    darkPalette->setColor(QPalette::Mid,             QColor( 37,  37,  37));
    darkPalette->setColor(QPalette::Shadow,          QColor( 0,    0,   0));

    darkPalette->setColor(QPalette::Disabled, QPalette::Text, QColor(127, 127, 127));
    darkPalette->setColor(QPalette::Disabled, QPalette::Window, QColor(100, 100, 100));
    darkPalette->setColor(QPalette::Disabled, QPalette::Base, QColor(100, 100, 100));
    darkPalette->setColor(QPalette::Disabled, QPalette::Button, QColor(100, 100, 100));

    return (*darkPalette);
}

void MainWindow::setPlaying(bool playing)
{   isPlaying = playing;    }

bool MainWindow::frameStarted(const Ogre::FrameEvent &evt)
{    return true;   }

bool MainWindow::frameRenderingQueued(const Ogre::FrameEvent &evt)
{
    // Set animation
    if(isPlaying && SelectionSet::getSingleton()->hasEntities())
    {
        for(Ogre::Entity const* ent : SelectionSet::getSingleton()->getEntitiesSelectionList())
        {
            Ogre::AnimationStateSet const *set = ent->getAllAnimationStates();
            for(const auto& [key, value] : set->getAnimationStates())
            {
                value->addTime(evt.timeSinceLastFrame);
            }
        }
    }
    return true;
}

bool MainWindow::frameEnded(const Ogre::FrameEvent &evt)
{
    if(mUriList.size())
    {
        importMeshs(mUriList);
        mUriList.clear();
    }

    for(EditorViewport* editorViewport: mDockWidgetList )
        editorViewport->getOgreWidget()->update();

    //Update the status bar
    QString statusMessage = "Status ";
    if(SelectionSet::getSingleton()->hasNodes())
        statusMessage += "Working with Nodes - to change the mesh position, select only the mesh";
    else if(SelectionSet::getSingleton()->hasEntities())
        statusMessage += "Working with Mesh";

    ui->statusBar->showMessage(statusMessage);

    return true;
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    QtInputManager::getInstance().keyPressEvent(event);

    switch(event->key()){
    case Qt::Key_R:
        setTransformState(TransformOperator::TS_ROTATE);
       break;
    case Qt::Key_Y:
        setTransformState(TransformOperator::TS_SELECT);
       break;
    case Qt::Key_T:
        setTransformState(TransformOperator::TS_TRANSLATE);
       break;
    case Qt::Key_Delete:
        TransformOperator::getSingleton()->removeSelected();
       break;
    default:
        // We hit a non mapped key !
       break;
    }

}

void MainWindow::keyReleaseEvent(QKeyEvent *event)
{
    QtInputManager::getInstance().keyReleaseEvent(event);
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QString mime = event->mimeData()->data("text/uri-list");
    QStringList uris = mime.split("\n",Qt::SkipEmptyParts);

    for(int c=uris.size()-1;c>=0;--c)
    {
        QString uri = uris.at(c);
        uri=uri.remove(0,8);
        uri.chop(1);

        if(Manager::getSingleton()->isValidFileExtention(uri))
        {
            uri.replace("%20"," ");
            uris.replace(c,uri);
        }
        else
        {
            uris.removeAt(c);
        }
    }
    mUriList.append(uris);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    event->acceptProposedAction();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void MainWindow::on_actionImport_triggered()
{
    QStringList fileNames = QFileDialog::getOpenFileNames(this, tr("Select a mesh file to import"),
                                                     "",
                                                     QString("Model ( "+ Manager::getSingleton()->getValidFileExtention().replace(".","*.") + " )"),
                                                     nullptr, QFileDialog::DontUseNativeDialog|QFileDialog::HideNameFilterDetails);

    mUriList.append(fileNames);
}

void MainWindow::importMeshs(const QStringList &_uriList)
{
    MeshImporterExporter::importer(_uriList/*, &lastImported*/);
}

void MainWindow::on_actionExport_Selected_triggered()
{
    if(SelectionSet::getSingleton()->hasNodes())
    {
        foreach(Ogre::SceneNode* node, SelectionSet::getSingleton()->getNodesSelectionList())
            MeshImporterExporter::exporter(node);
    }
}

void MainWindow::on_actionMaterial_Editor_triggered()
{
    auto m = new Material(this);

    QStringList List;
    Ogre::ResourceManager::ResourceMapIterator materialIterator = Ogre::MaterialManager::getSingleton().getResourceIterator();
    while (materialIterator.hasMoreElements())
    {
        List.append(Ogre::static_pointer_cast<Ogre::Material>(materialIterator.peekNextValue())->getName().data());

        materialIterator.moveNext();
    }
    m->setWindowTitle("Material List");
    m->SetMaterialList(List);

    m->show();
}

void MainWindow::on_actionAbout_triggered()
{
    About *m = new About(this);
    m->show();
}

void MainWindow::on_actionObjects_Toolbar_toggled(bool arg1)
{    ui->objectsToolbar->setVisible(arg1);  }

void MainWindow::on_actionTools_Toolbar_toggled(bool arg1)
{    ui->toolToolbar->setVisible(arg1); }

void MainWindow::on_actionMeshEditor_toggled(bool arg1)
{    ui->meshEditorWidget->setVisible(arg1);    }

void MainWindow::chooseBgColor()
{
    if(!mDockWidgetList.isEmpty())
    {
        QColor prevColor =  mDockWidgetList.at(0)->getOgreWidget()->getBackgroundColor();
        QColor c = QColorDialog::getColor(prevColor, this, tr("Choose background color"), QColorDialog::DontUseNativeDialog);
        if(c.isValid()){
            foreach(EditorViewport* pDockWidget, mDockWidgetList)
                pDockWidget->getOgreWidget()->setBackgroundColor(c);
        }
    }
    else
    {
        QMessageBox::warning(this,
                             tr("An exception has occured!"),
                             tr("Impossible to set a background color :\nNo viewport is open."));
    }

}

void MainWindow::setTransformState(TransformOperator::TransformState newState)
{
    switch ( newState ) {
    case TransformOperator::TS_SELECT:
        ui->actionSelect_Object->setChecked(true);
        ui->actionTranslate_Object->setChecked(false);
        ui->actionRotate_Object->setChecked(false);
      break;
    case TransformOperator::TS_TRANSLATE:
        ui->actionSelect_Object->setChecked(false);
        ui->actionTranslate_Object->setChecked(true);
        ui->actionRotate_Object->setChecked(false);
      break;
    case TransformOperator::TS_ROTATE:
        ui->actionSelect_Object->setChecked(false);
        ui->actionTranslate_Object->setChecked(false);
        ui->actionRotate_Object->setChecked(true);
      break;
    default:
        ui->actionSelect_Object->setChecked(false);
        ui->actionTranslate_Object->setChecked(false);
        ui->actionRotate_Object->setChecked(false);
      break;
    }
    TransformOperator::getSingleton()->onTransformStateChange(static_cast<TransformOperator::TransformState> (newState));
}

void MainWindow::createEditorViewport(/*TODO add the type of view (perspective, left,....*/)
{
    //Finding the first (lower number) available index in the list
    int nextIndex = 1;
    QList<EditorViewport*>::iterator widgetIterator;
    widgetIterator = mDockWidgetList.begin();
    while((widgetIterator < mDockWidgetList.end())
          && ((*widgetIterator)->getIndex() == nextIndex))
    {
        ++widgetIterator;
        ++nextIndex;
    }

    if(widgetIterator!=mDockWidgetList.end())
        ++widgetIterator;

    //Creating Docked Main widget;
    EditorViewport* pOgreViewport = new EditorViewport(this, nextIndex);
    //OgreWidget* pOgreWidget = pOgreViewport->getOgreWidget();

    connect(pOgreViewport, SIGNAL(widgetAboutToClose(EditorViewport* const&)), this, SLOT(onWidgetClosing(EditorViewport* const&)));
    connect(pOgreViewport->getOgreWidget(), SIGNAL(focusOnWidget(OgreWidget*)), TransformOperator::getSingleton(), SLOT(setActiveWidget(OgreWidget*)));

    if(!mDockWidgetList.isEmpty())
    {
        QColor c =  mDockWidgetList.at(0)->getOgreWidget()->getBackgroundColor();
        pOgreViewport->getOgreWidget()->setBackgroundColor(c);
    }

    //We insert the widget in the coorect place in the list so that the list is ordered
    mDockWidgetList.insert(widgetIterator, pOgreViewport);

    //before adding, we look where are the other ones

//    QList<Qt::DockWidgetArea> existingWidgetPosList;
//    foreach (OgreWidget* pOgreWidget, mOgreWidgetList)
//        existingWidgetPosList.append(dockWidgetArea(pOgreWidget));

    //dock->setWidget(pOgreWidget);

    addDockWidget(Qt::LeftDockWidgetArea,pOgreViewport);

    // TODO add some procedure to determine where to create the new widget so that it looks like 2x2 matrix view
    // it should determine the position of the existing Docked Widget

    ui->actionSingle->blockSignals(true);
    ui->action1x1_Side_by_Side->blockSignals(true);
    ui->action1x1_Upper_and_Lower->blockSignals(true);
    ui->actionSingle->setChecked(false);
    ui->action1x1_Side_by_Side->setChecked(false);
    ui->action1x1_Upper_and_Lower->setChecked(false);
    ui->actionSingle->blockSignals(false);
    ui->action1x1_Side_by_Side->blockSignals(false);
    ui->action1x1_Upper_and_Lower->blockSignals(false);
}

void MainWindow::onWidgetClosing(EditorViewport* const& widget)
{
    // Artificial MUTEX !!! don't know if required
    m_pTimer->stop();
    bool result = mDockWidgetList.removeOne(widget);

    if(result)
        delete widget;
    else
        qDebug()<<"Unable to remove viewport "<<widget->getIndex();
    m_pTimer->start(0);
}

void MainWindow::on_actionSingle_toggled(bool arg1)
{
    if(arg1)
    {
        while(mDockWidgetList.size()>1)
        {
            mDockWidgetList.last()->close();
        }
        ui->actionSingle->setChecked(true);
        ui->action1x1_Side_by_Side->setChecked(false);
        ui->action1x1_Upper_and_Lower->setChecked(false);
    } else { //Doesn't allow unchecking
        ui->actionSingle->setChecked(   !ui->action1x1_Side_by_Side->isChecked() &&
                                        !ui->action1x1_Upper_and_Lower->isChecked());
    }
}

void MainWindow::on_action1x1_Side_by_Side_toggled(bool arg1)
{
    if(arg1)
    {
        while(mDockWidgetList.size()<2)
        {
            createEditorViewport();
        }
        while(mDockWidgetList.size()>2)
        {
            mDockWidgetList.last()->close();
        }

        splitDockWidget(mDockWidgetList.first(),mDockWidgetList.last(),Qt::Horizontal);

        ui->actionSingle->setChecked(false);
        ui->action1x1_Side_by_Side->setChecked(true);
        ui->action1x1_Upper_and_Lower->setChecked(false);
    } else { //Doesn't allow unchecking
        ui->action1x1_Side_by_Side->setChecked( !ui->actionSingle->isChecked() &&
                                                !ui->action1x1_Upper_and_Lower->isChecked());
    }
}

void MainWindow::on_action1x1_Upper_and_Lower_toggled(bool arg1)
{
    if(arg1)
    {
        while(mDockWidgetList.size()<2)
        {
            createEditorViewport();
        }
        while(mDockWidgetList.size()>2)
        {
            mDockWidgetList.last()->close();
        }

        splitDockWidget(mDockWidgetList.first(),mDockWidgetList.last(),Qt::Horizontal);
        splitDockWidget(mDockWidgetList.first(),mDockWidgetList.last(),Qt::Vertical);

        ui->actionSingle->setChecked(false);
        ui->action1x1_Side_by_Side->setChecked(false);
        ui->action1x1_Upper_and_Lower->setChecked(true);
    } else { //Doesn't allow unchecking
        ui->action1x1_Upper_and_Lower->setChecked(  !ui->actionSingle->isChecked() &&
                                                    !ui->action1x1_Upper_and_Lower->isChecked());
    }
}

void MainWindow::on_actionAdd_Resource_location_triggered()
{
    try{
        QString path = QFileDialog::getExistingDirectory(this, "", "", QFileDialog::DontUseNativeDialog|QFileDialog::ShowDirsOnly);

        try{
            Ogre::ResourceGroupManager::getSingleton().destroyResourceGroup(path.toStdString().data());
        }catch(...){}

        Ogre::ResourceGroupManager::getSingleton().createResourceGroup(path.toStdString().data());
        Ogre::ResourceGroupManager::getSingleton().addResourceLocation(path.toStdString().data(),"FileSystem",path.toStdString().data(),false, true);
        Ogre::ResourceGroupManager::getSingleton().initialiseAllResourceGroups();
    }catch(const Ogre::Exception& ex)
    {
       QMessageBox::critical(this, "Error on loading resources", ex.what());
    }
}

void MainWindow::on_actionChange_Ambient_Light_triggered()
{
    Ogre::ColourValue c = Manager::getSingleton()->getSceneMgr()->getAmbientLight();
    ambientLightColorDialog->setCurrentColor(QColor(c.r*255,c.g*255,c.b*255,c.a*255));
    ambientLightColorDialog->show();
}

void MainWindow::on_actionLight_toggled(bool arg1)
{
    if(arg1){
        QApplication::setPalette(QColor("ghostwhite"));
        QSettings settings;
        settings.setValue("palette","light");
        ui->actionDark->setChecked(false);
        ui->actionCustom->blockSignals(true);
        ui->actionCustom->setChecked(false);
        ui->actionCustom->blockSignals(false);
    } else { //Doesn't allow unchecking
        ui->actionLight->setChecked(!ui->actionDark->isChecked() &&
                                   !ui->actionCustom->isChecked());
    }
}

void MainWindow::on_actionDark_toggled(bool arg1)
{
    if(arg1){
        QApplication::setPalette(darkPalette());

        QSettings settings;
        settings.setValue("palette","dark");
        ui->actionLight->setChecked(false);
        ui->actionCustom->blockSignals(true);
        ui->actionCustom->setChecked(false);
        ui->actionCustom->blockSignals(false);
    } else { //Doesn't allow unchecking
        ui->actionDark->setChecked(!ui->actionLight->isChecked() &&
                                   !ui->actionCustom->isChecked());
    }
}

void MainWindow::on_actionCustom_toggled(bool arg1)
{
    customPaletteColorDialog->show();
    ui->actionCustom->blockSignals(true);
    if(arg1){
        ui->actionCustom->setChecked(false);
    } else { //Doesn't allow unchecking
        ui->actionCustom->setChecked(!ui->actionLight->isChecked() &&
                                   !ui->actionDark->isChecked());
    }
    ui->actionCustom->blockSignals(false);
}

void MainWindow::custom_Palette_Color_Selected(const QColor &color)
{
    QApplication::setPalette(color);
    QSettings settings;
    settings.setValue("palette","custom");
    settings.setValue("customPalette",color);
    ui->actionCustom->blockSignals(true);
    ui->actionCustom->setChecked(true);
    ui->actionLight->setChecked(false);
    ui->actionDark->setChecked(false);
    ui->actionCustom->blockSignals(false);
}

void MainWindow::on_actionVerify_Update_triggered()
{
    // Verify if the latest release on GitHub is equal to the current version
    // If not, ask the user if he wants to update
    // If yes, download the latest release and install it

    auto networkManager = new QNetworkAccessManager(this);

    // Send a GET request to the GitHub API to retrieve the latest release information
    QNetworkRequest request(QUrl("https://api.github.com/repos/fernandotonon/QtMeshEditor/releases/latest"));
    QNetworkReply* reply = networkManager->get(request);

    // Connect the finished signal to a slot to handle the response
    connect(reply, &QNetworkReply::finished, this, [=]() {
        if (reply->error() == QNetworkReply::NoError) {
            // Read the response data
            QByteArray data = reply->readAll();

            // Parse the JSON response
            QJsonDocument doc = QJsonDocument::fromJson(data);
            QJsonObject obj = doc.object();

            // Get the tag name of the latest release
            QString latestVersion = obj.value("tag_name").toString();
            QString currentVersion = QApplication::applicationVersion();
            if (latestVersion == currentVersion) {
                // The latest release is equal to the current version
                QMessageBox::information(nullptr, tr("Update"), tr("You're using the latest release."));
            } else {
                // The latest release is different from the current version
                QMessageBox::StandardButton reply = QMessageBox::question(nullptr, tr("Update"), tr("A new version is available. Do you want to update?"), QMessageBox::Yes | QMessageBox::No);
                // if yes, open the download link in the default browser
                if (reply == QMessageBox::Yes) {
                    QString downloadUrl = obj.value("html_url").toString();
                    QDesktopServices::openUrl(QUrl(downloadUrl));
                }
            }
        } else {
            // Handle the error
            Ogre::LogManager::getSingleton().logMessage(reply->errorString().toStdString());
        }

        // Clean up
        networkManager->deleteLater();
        reply->deleteLater();
    });

    // Connect the SSL errors signal to a slot to handle SSL errors
    connect(networkManager, &QNetworkAccessManager::sslErrors, this, [=](QNetworkReply* reply, const QList<QSslError>& errors) {
        // Ignore SSL errors
        reply->ignoreSslErrors();
    });
}

