#include <QMessageBox>
#ifndef Q_OS_WIN
#include <unistd.h>
#endif
#include <QSettings>
#include <QApplication>
#include <QLibraryInfo>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonArray>
#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QJSEngine>
#include <QQuickWindow>
#include <QFileInfo>
#include "SentryReporter.h"
#include <QDialog>
#include <QProgressDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
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
#include "AnimationWidget.h"
#include "AnimationMerger.h"
#include "SelectionSet.h"
#include "AnimationControlController.h"
#include "MaterialEditorQML.h"
#include "LLMSettingsWidget.h"
#include "MCPSettingsDialog.h"
#include "MCPServer.h"
#include "NormalVisualizer.h"
#include "MeshInfoOverlay.h"
#include "SpaceCamera.h"
#include "ViewCube/ViewCubeController.h"
#include "LLMManager.h"
#include "QMLMaterialHighlighter.h"
#include "ModelDownloader.h"
#include "UndoManager.h"
#include "PropertiesPanelController.h"
#include "MeshLodController.h"
#include "MeshValidator.h"
#include "MaterialPresetLibrary.h"
#include "AIChatManager.h"
#include <QDockWidget>
#include <QQuickWidget>
#include <QQmlContext>

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

    // Right dock (Inspector) takes full height — bottom dock stops at the right dock boundary
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);

    Manager* manager = Manager::getSingleton(this); // init the Ogre Root/RenderSystem/SceneManager

    createEditorViewport(/*TODO add the type of view (perspective, left,....*/);

    manager->loadResources(); // Resources should be loaded after createRenderWindow...

    m_pRoot = manager->getRoot();
    m_pRoot->addFrameListener(this);

    manager->CreateEmptyScene();

    initToolBar();

    // Recent Files submenu in File menu
    m_recentFilesMenu = new QMenu(tr("Recent Files"), this); // NOSONAR — Qt parent manages lifetime
    m_recentFilesMenu->setObjectName("recentFilesMenu");
    ui->menuFile->insertMenu(ui->actionExport_Selected, m_recentFilesMenu);
    ui->menuFile->insertSeparator(ui->actionExport_Selected);
    updateRecentFilesMenu();

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
        if(m_pRoot && m_pRoot->getRenderSystem())
        {
            try {
                m_pRoot->renderOneFrame();
            } catch (Ogre::Exception& e) {
                fprintf(stderr, "RENDER ERROR (Ogre): %s\n", e.getFullDescription().c_str());
                SentryReporter::captureMessage(
                    QString("Render error (Ogre): %1").arg(e.getFullDescription().c_str()), "error");
                if(m_pTimer) m_pTimer->stop();
            } catch (std::exception& e) {
                fprintf(stderr, "RENDER ERROR (std): %s\n", e.what());
                SentryReporter::captureMessage(
                    QString("Render error (std): %1").arg(e.what()), "error");
                if(m_pTimer) m_pTimer->stop();
            } catch (...) {
                fprintf(stderr, "RENDER ERROR (unknown)\n");
                SentryReporter::captureMessage("Render error (unknown)", "error");
                if(m_pTimer) m_pTimer->stop();
            }
        }
    });
    m_pTimer->start(0);

    // Auto-start MCP HTTP server if enabled in settings
    QSettings mcpSettings;
    bool mcpEnabled = mcpSettings.value("MCP/enabled", false).toBool();
    int mcpPort = mcpSettings.value("MCP/port", 8080).toInt();
    if (mcpEnabled && !m_mcpServer) {
        startMCPServer(mcpPort);
    }
}

/////////////////////////// TODO Clean up the code of MainWindow
/// /////////////////////// TODO improve the ui (toolbar, menubar,....) and add translation (obviously Portuguese but french, english, may be japaneese !)
MainWindow::~MainWindow()
{
    // Destroy overlays early — they connect to Manager signals and
    // access Ogre resources, so they must be deleted while Manager is alive.
    // ViewCubeController is parented to this, no manual delete needed
    m_viewCubeController = nullptr;

    delete m_meshInfoOverlay;
    m_meshInfoOverlay = nullptr;
    delete m_normalVisualizer;
    m_normalVisualizer = nullptr;

    // Stop MCP server if running
    if (m_mcpServer) {
        m_mcpServer->stop();
        delete m_mcpServer;
        m_mcpServer = nullptr;
    }

    // CRITICAL: Stop the timer FIRST to prevent any renderOneFrame() calls
    // during shutdown. This prevents swap buffer errors when windows are destroyed.
    if(m_pTimer)
    {
        m_pTimer->stop();
        m_pTimer->disconnect(); // Disconnect all signals to prevent any pending calls
        delete m_pTimer;
        m_pTimer = nullptr;
    }

    // Safely remove frame listener if root still exists (before destroying widgets)
    if(m_pRoot)
    {
        try {
            m_pRoot->removeFrameListener(this);
        } catch (...) {
            // Ignore exceptions during shutdown
        }
    }

    // Now safely close all viewports (their windows can be destroyed without rendering issues)
    // IMPORTANT: During MainWindow destruction, we must NOT call close() which emits signals
    // Instead, we delete the widgets directly to avoid signal/slot issues during destruction
    // IMPORTANT: Destroy widgets BEFORE destroying Manager to ensure they can safely
    // detach from OGRE resources while Manager still exists
    foreach (EditorViewport* pOgreWidget, mDockWidgetList)
    {
        if(pOgreWidget)
        {
            // Disconnect signals to prevent onWidgetClosing from being called during destruction
            disconnect(pOgreWidget, nullptr, this, nullptr);
            // Delete directly instead of calling close() to avoid signal emission
            delete pOgreWidget;
        }
    }
    mDockWidgetList.clear();

    delete ui;
    // m_pTransformWidget removed — replaced by QML Inspector panel
    if(m_pPrimitivesWidget)
    {
        delete m_pPrimitivesWidget;
        m_pPrimitivesWidget = nullptr;
    }
    
    // CRITICAL: Destroy Manager AFTER all widgets that depend on it are destroyed
    // This ensures that OGRE resources are cleaned up in the correct order
    // The Manager will clean up all OGRE resources (scene manager, root, etc.)
    // Only destroy Manager if it still exists and belongs to this MainWindow
    // (In tests, Manager may be destroyed separately in TearDown)
    // These singletons are safe to destroy unconditionally.
    AnimationControlController::kill();
    MeshLodController::kill();
    MeshValidator::kill();
    MaterialPresetLibrary::kill();
    AIChatManager::kill();
    // Only destroy Manager if it still exists and belongs to this MainWindow
    // (In tests, Manager may be destroyed separately in TearDown)
    Manager* manager = Manager::getSingletonPtr();
    if(manager && manager->getMainWindow() == this)
        Manager::kill();
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

    setTransformState(TransformOperator::TS_SELECT);
    connect(ui->actionSelect_Object, &QAction::triggered, this, [this]{setTransformState(TransformOperator::TS_SELECT);});
    connect(ui->actionTranslate_Object, &QAction::triggered, this, [this]{setTransformState(TransformOperator::TS_TRANSLATE);});
    connect(ui->actionRotate_Object, &QAction::triggered, this, [this]{setTransformState(TransformOperator::TS_ROTATE);});
    connect(ui->actionScale_Object, &QAction::triggered, this, [this]{setTransformState(TransformOperator::TS_SCALE);});
    connect(ui->actionRemove_Object, SIGNAL(triggered()), TransformOperator::getSingleton(), SLOT(removeSelected()));
    connect(ui->actionToggle_Transform_Space, &QAction::toggled, this, [this](bool checked){
        TransformOperator::getSingleton()->setTransformSpace(
            checked ? TransformOperator::SPACE_LOCAL : TransformOperator::SPACE_WORLD);
    });
    connect(TransformOperator::getSingleton(), &TransformOperator::transformSpaceChanged, this, [this](TransformOperator::TransformSpace space){
        ui->actionToggle_Transform_Space->setChecked(space == TransformOperator::SPACE_LOCAL);
    });

    // Undo/Redo
    connect(ui->actionUndo, &QAction::triggered, UndoManager::getSingleton(), &UndoManager::undo);
    connect(ui->actionRedo, &QAction::triggered, UndoManager::getSingleton(), &UndoManager::redo);

    // Refresh gizmo position after undo/redo (deferred to avoid re-entrant scene access)
    connect(UndoManager::getSingleton()->stack(), &QUndoStack::indexChanged, this, [](int) {
        QTimer::singleShot(0, []() {
            // Re-trigger selection to update gizmo position and Inspector values
            auto* sel = SelectionSet::getSingleton();
            if (!sel->isEmpty())
                emit sel->selectionChanged();
        });
    });

    // QML Properties Panel (replaces old Transform tab with modern collapsible inspector)
    {
        // Force software rendering before creating any QQuickWidget to avoid GL conflicts with Ogre
        qputenv("QSG_RHI_BACKEND", "software");
        qputenv("QT_QUICK_BACKEND", "software");
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);

        m_propertiesPanel = new QQuickWidget();
        m_propertiesPanel->setResizeMode(QQuickWidget::SizeRootObjectToView);

        // Register QML singletons before setSource() so all imports resolve
        qmlRegisterSingletonType<PropertiesPanelController>("PropertiesPanel", 1, 0, "PropertiesPanelController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return PropertiesPanelController::qmlInstance(engine, nullptr);
            });
        qmlRegisterSingletonType<AnimationControlController>("AnimationControl", 1, 0, "AnimationControlController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return AnimationControlController::qmlInstance(engine, nullptr);
            });
        qmlRegisterSingletonType<MeshLodController>("PropertiesPanel", 1, 0, "MeshLodController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return MeshLodController::qmlInstance(engine, nullptr);
            });
        // Open the LOD export directory picker from MainWindow so the dialog has a
        // proper parent widget — QFileDialog invoked from a QML context doesn't
        // reliably appear on macOS without a valid parent QWidget.
        connect(MeshLodController::instance(), &MeshLodController::exportLodsRequested,
                this, [this](const QString& format) {
            // Defer via singleShot so QML's event processing finishes before the
            // native file picker opens (required on macOS to avoid invisible dialog).
            QTimer::singleShot(0, this, [this, format]() {
                QString dir = QFileDialog::getExistingDirectory(
                    this, "Export LOD levels to directory", QDir::homePath(),
                    QFileDialog::DontUseNativeDialog | QFileDialog::ShowDirsOnly);
                if (!dir.isEmpty())
                    MeshLodController::instance()->doExportLods(format, dir);
            });
        });
        qmlRegisterSingletonType<MeshValidator>("PropertiesPanel", 1, 0, "MeshValidator",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return MeshValidator::qmlInstance(engine, nullptr);
            });
        qmlRegisterSingletonType<MaterialPresetLibrary>("PropertiesPanel", 1, 0, "MaterialPresetLibrary",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return MaterialPresetLibrary::qmlInstance(engine, nullptr);
            });
        qmlRegisterSingletonType<AIChatManager>("AIChatPanel", 1, 0, "AIChatManager",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return AIChatManager::qmlInstance(engine, nullptr);
            });

        m_propertiesPanel->setSource(QUrl("qrc:/PropertiesPanel/PropertiesPanel.qml"));

        // Replace the tab widget content with the Inspector panel directly
        auto* dockContents = ui->meshEditorWidget->widget();
        auto* layout = dockContents->layout();
        // Remove old tab widget from layout
        if (layout) {
            QLayoutItem* item;
            while ((item = layout->takeAt(0)) != nullptr) {
                if (item->widget())
                    item->widget()->hide();
                delete item;
            }
            layout->addWidget(m_propertiesPanel);
        }
    }

    // AI Chat dock
    {
        auto* chatWidget = new QQuickWidget();
        chatWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
        chatWidget->setMinimumWidth(280);
        chatWidget->setMinimumHeight(350);
        // StrongFocus: a single click inside the dock routes keyboard events into QML
        // without requiring a prior click in the viewport.
        chatWidget->setFocusPolicy(Qt::StrongFocus);
        chatWidget->setSource(QUrl("qrc:/AIChatPanel/AIChatPanel.qml"));
        m_chatDock = new QDockWidget(tr("AI Chat"), this);
        m_chatDock->setWidget(chatWidget);
        m_chatDock->setObjectName("AIChatDock");
        addDockWidget(Qt::RightDockWidgetArea, m_chatDock);
        resizeDocks({m_chatDock}, {400}, Qt::Vertical);
        m_chatDock->hide();
    }

    // Animation Control dock is created below and auto-shown when animated entity is selected

    // PrimitivesWidget (hidden — used by toolbar create menu and Inspector primitive editing)
    m_pPrimitivesWidget = new PrimitivesWidget(this);
    m_pPrimitivesWidget->hide();

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

    // AI Chat button — star icon is the common AI shorthand
    auto aiChatButton = new QToolButton(ui->objectsToolbar);
    aiChatButton->setText("\u2728");  // ✨
    aiChatButton->setToolTip(tr("Open AI Chat"));
    QFont aiFont = aiChatButton->font();
    aiFont.setPixelSize(15);
    aiChatButton->setFont(aiFont);
    connect(aiChatButton, &QToolButton::clicked, this, [this]() {
        if (m_chatDock) {
            m_chatDock->show();
            m_chatDock->raise();
        }
    });
    ui->objectsToolbar->addWidget(aiChatButton);

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

    // AnimationWidget (hidden — used by Inspector for skeleton/weight toggles)
    auto pAnimationWidget = new AnimationWidget(this);
    pAnimationWidget->hide();

    // Rename signal from AnimationWidget still triggers a tree refresh
    connect(pAnimationWidget, SIGNAL(changeAnimationName(const std::string&)),
            AnimationControlController::instance(), SLOT(updateAnimationTree()));

    connect(pAnimationWidget,SIGNAL(changeAnimationState(bool)),this,SLOT(setPlaying(bool)));

    // Give PropertiesPanelController access to AnimationWidget for skeleton/weight toggles
    PropertiesPanelController::instance()->setAnimationWidget(pAnimationWidget);

    // Connect Inspector's playing state to MainWindow animation playback
    connect(PropertiesPanelController::instance(), &PropertiesPanelController::playingChanged, this, [this]() {
        setPlaying(PropertiesPanelController::instance()->isPlaying());
    });

    // Merge Animations button — enable/disable based on selection
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, &MainWindow::updateMergeAnimationsButton);

    // Viewport
    connect(ui->actionAdd_Viewport, SIGNAL(triggered()), this, SLOT(createEditorViewport()));
    connect(ui->actionChange_BG_Color, SIGNAL(triggered()), this, SLOT(chooseBgColor()));

    // show grid
    connect(ui->actionShow_Grid, SIGNAL(toggled(bool)),Manager::getSingleton()->getViewportGrid(),SLOT(setVisible(bool)));

    // show normals
    m_normalVisualizer = new NormalVisualizer(Manager::getSingleton()->getSceneMgr(), this);
    connect(ui->actionShow_Normals, &QAction::toggled, m_normalVisualizer, &NormalVisualizer::setVisible);

    // show mesh info overlay
    m_meshInfoOverlay = new MeshInfoOverlay(this);
    connect(ui->actionShow_Mesh_Info, &QAction::toggled, m_meshInfoOverlay, &MeshInfoOverlay::setVisible);
    // Sync menu checkmark when MCP or other code toggles the overlay directly
    connect(m_meshInfoOverlay, &MeshInfoOverlay::visibilityChanged, ui->actionShow_Mesh_Info, &QAction::setChecked);
    // Connect viewports created before the overlay existed
    for (EditorViewport* vp : mDockWidgetList)
        connect(vp->getOgreWidget(), &OgreWidget::focusOnWidget, m_meshInfoOverlay, &MeshInfoOverlay::setActiveWidget);

    // ViewCube (3D navigation gizmo) — top-level window positioned over the active viewport
    m_viewCubeController = new ViewCubeController(this);
    // Force software rendering for the ViewCube QML widget (avoid GL conflicts with Ogre)
    qputenv("QSG_RHI_BACKEND", "software");
    qputenv("QT_QUICK_BACKEND", "software");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    m_viewCubeController->initWidget();

    connect(ui->actionShow_View_Cube, &QAction::toggled, m_viewCubeController, &ViewCubeController::setVisible);
    connect(m_viewCubeController, &ViewCubeController::visibilityChanged, ui->actionShow_View_Cube, &QAction::setChecked);

    // Connect viewports created before the ViewCube existed
    for (EditorViewport* vp : mDockWidgetList)
        connect(vp->getOgreWidget(), &OgreWidget::focusOnWidget, this, [this](OgreWidget* w) {
            m_viewCubeController->setActiveWidget(w);
        });

    // Activate the first viewport, then set visible
    // (setActiveWidget emits visibilityChanged which checks isVisible(),
    //  so m_visible must be true AND the widget must be visible)
    if (!mDockWidgetList.isEmpty())
        m_viewCubeController->setActiveWidget(mDockWidgetList.first()->getOgreWidget());
    m_viewCubeController->setVisible(true);

    // AI Settings menu
    QMenu* aiMenu = menuBar()->addMenu(tr("&AI"));
    QAction* aiChatAction = aiMenu->addAction(QIcon(":/icones/ai.png"), tr("AI Chat..."));
    connect(aiChatAction, &QAction::triggered, this, [this]() {
        if (m_chatDock) {
            m_chatDock->show();
            m_chatDock->raise();
        }
    });
    aiMenu->addSeparator();
    QAction* aiSettingsAction = aiMenu->addAction(tr("AI Model Settings..."));
    connect(aiSettingsAction, &QAction::triggered, this, &MainWindow::showAIModelSettings);

    QAction* mcpSettingsAction = aiMenu->addAction(tr("MCP Server Settings..."));
    connect(mcpSettingsAction, &QAction::triggered, this, &MainWindow::showMCPSettings);

    // Crash reporting toggle in Help menu
    ui->menuHelp->addSeparator();
    QAction* crashReportAction = ui->menuHelp->addAction(tr("Send Crash Reports"));
    crashReportAction->setCheckable(true);
    crashReportAction->setChecked(SentryReporter::isEnabled());
    connect(crashReportAction, &QAction::toggled, this, [](bool checked) {
        SentryReporter::setEnabled(checked);
        if (checked) {
            QMessageBox::information(nullptr, QObject::tr("Crash Reporting"),
                QObject::tr("Crash reporting will be enabled on next launch."));
        }
    });

    // Initialize LLMManager
    LLMManager::instance();
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

// LCOV_EXCL_START — Ogre frame listener requires render loop
bool MainWindow::frameStarted(const Ogre::FrameEvent &evt)
{    return true;   }

bool MainWindow::frameRenderingQueued(const Ogre::FrameEvent &evt)
{
    // Advance time for every entity that has enabled animation states
    if(isPlaying)
    {
        for(Ogre::SceneNode* node : Manager::getSingleton()->getSceneNodes())
        {
            if(!node) continue;
            for(int i = 0; i < static_cast<int>(node->numAttachedObjects()); ++i)
            {
                Ogre::MovableObject* obj = node->getAttachedObject(i);
                if(!obj || obj->getMovableType() != "Entity") continue;

                auto* ent = static_cast<Ogre::Entity*>(obj);
                Ogre::AnimationStateSet const* set = ent->getAllAnimationStates();
                if(!set) continue;
                for(const auto& [key, value] : set->getAnimationStates())
                {
                    if(value->getEnabled())
                        value->addTime(evt.timeSinceLastFrame);
                }
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

    // Update ViewCube orientation from camera
    if (m_viewCubeController)
        m_viewCubeController->updateOrientation();

    //Update the status bar
    QString statusMessage;

    // Transform mode
    auto* tOp = TransformOperator::getSingleton();
    switch (tOp->getTransformSpace()) {
    case TransformOperator::SPACE_WORLD: statusMessage += "World | "; break;
    case TransformOperator::SPACE_LOCAL: statusMessage += "Local | "; break;
    }

    // Selection info
    auto* sel = SelectionSet::getSingleton();
    if (sel->hasNodes())
        statusMessage += QString("Nodes: %1").arg(sel->getNodesCount());
    else if (sel->hasEntities())
        statusMessage += QString("Entities: %1").arg(sel->getEntitiesCount());
    else if (sel->hasSubEntities())
        statusMessage += QString("Submeshes: %1").arg(sel->getSubEntitiesCount());
    else
        statusMessage += "No selection";

    ui->statusBar->showMessage(statusMessage);

    return true;
}
// LCOV_EXCL_STOP

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    QtInputManager::getInstance().keyPressEvent(event);

    switch(event->key()){
    case Qt::Key_Q:
        setTransformState(TransformOperator::TS_SELECT);
       break;
    case Qt::Key_W:
        setTransformState(TransformOperator::TS_TRANSLATE);
       break;
    case Qt::Key_E:
        setTransformState(TransformOperator::TS_ROTATE);
       break;
    case Qt::Key_R:
        setTransformState(TransformOperator::TS_SCALE);
       break;
    case Qt::Key_F:
    {
        // Frame selection: zoom camera to fit selected objects
        SpaceCamera* cam = nullptr;
        for (auto* vp : mDockWidgetList) {
            if (vp->getOgreWidget()->hasFocus()) {
                cam = vp->getOgreWidget()->getSpaceCamera();
                break;
            }
        }
        if (!cam && !mDockWidgetList.isEmpty())
            cam = mDockWidgetList.first()->getOgreWidget()->getSpaceCamera();
        if (cam) cam->frameSelection();
        break;
    }
    case Qt::Key_X:
        TransformOperator::getSingleton()->toggleTransformSpace();
       break;
    case Qt::Key_Delete:
        TransformOperator::getSingleton()->removeSelected();
       break;
    default:
       break;
    }

}

void MainWindow::keyReleaseEvent(QKeyEvent *event)
{
    QtInputManager::getInstance().keyReleaseEvent(event);
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList validFiles;
    for (const QUrl& url : event->mimeData()->urls())
    {
        QString filePath = url.toLocalFile();
        if (!filePath.isEmpty() && Manager::getSingleton()->isValidFileExtention(filePath))
            validFiles.append(filePath);
    }
    for (const QString& f : validFiles)
        addToRecentFiles(f);
    mUriList.append(validFiles);
}

// LCOV_EXCL_START — already tested via keyPressEvent
void MainWindow::closeEvent(QCloseEvent *event)
{
    // Shut down LLM worker to release model resources
    LLMManager::instance()->shutdownWorkerThread();

    // Process pending deletions and let Ogre shut down cleanly
    QApplication::quit();
    QMainWindow::closeEvent(event);

    // Use _exit() to skip static destructors — ggml-metal's global device cleanup
    // asserts if any compute pipeline sets remain, but there's no public API to
    // release them. CLIPipeline uses the same workaround. See:
    // https://github.com/ggml-org/llama.cpp/pull/17869
#ifdef Q_OS_MACOS
    _exit(0);
#endif
}
// LCOV_EXCL_STOP

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    event->acceptProposedAction();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
// LCOV_EXCL_START — opens QFileDialog
void MainWindow::on_actionImport_triggered()
{
    SentryReporter::addBreadcrumb("ui.action", "Import mesh files");

    QStringList fileNames = QFileDialog::getOpenFileNames(this, tr("Select a mesh file to import"),
                                                     "",
                                                     QString("Model ( "+ Manager::getSingleton()->getValidFileExtention().replace(".","*.") + " )"),
                                                     nullptr, QFileDialog::DontUseNativeDialog|QFileDialog::HideNameFilterDetails);

    for (const QString& f : fileNames)
        addToRecentFiles(f);
    mUriList.append(fileNames);
}
// LCOV_EXCL_STOP

void MainWindow::importMeshs(const QStringList &_uriList)
{
    auto txn = SentryReporter::startTransaction("ui.import", "file.import");
    QList<Ogre::SkeletonPtr> animOnlySkeletons;
    try {
        MeshImporterExporter::importer(_uriList, 0, &animOnlySkeletons);
    } catch (...) {
        SentryReporter::finishTransaction(txn);
        throw;
    }
    SentryReporter::finishTransaction(txn);

    // Handle animation-only files: show a notification and offer an immediate merge
    // if a compatible entity is already selected.
    for (const Ogre::SkeletonPtr& skel : animOnlySkeletons) {
        if (!skel) continue;
        SentryReporter::addBreadcrumb("import",
            QString("Animation-only file detected: %1 animation(s)").arg(skel->getNumAnimations()));
        unsigned short numAnims = skel->getNumAnimations();
        QString animList;
        for (unsigned short i = 0; i < numAnims; ++i)
            animList += "\n  \u2022 " + QString::fromStdString(skel->getAnimation(i)->getName());

        QString displayName = QString::fromStdString(skel->getName());
        if (displayName.endsWith(".skeleton", Qt::CaseInsensitive))
            displayName.chop(9);

        Ogre::Entity* baseEntity = nullptr;
        auto selected = SelectionSet::getSingleton()->getResolvedEntities();
        for (Ogre::Entity* e : selected) {
            if (e && e->hasSkeleton()) { baseEntity = e; break; }
        }

        if (baseEntity) {
            auto btn = QMessageBox::question(this, "Animation-only file",
                QString("'%1' contains no mesh geometry \u2014 %2 animation(s) were found:%3\n\n"
                        "Merge these animations into '%4'?")
                    .arg(displayName).arg(numAnims).arg(animList)
                    .arg(baseEntity->getName().c_str()),
                QMessageBox::Yes | QMessageBox::No);
            if (btn == QMessageBox::Yes) {
                SentryReporter::addBreadcrumb("ui.action", "Merge animation-only file into selected mesh");
                QString errMsg;
                Ogre::Entity* merged = AnimationMerger::mergeAnimations(baseEntity, {}, {skel}, errMsg);
                if (!merged) {
                    QMessageBox::warning(this, "Merge failed",
                        errMsg.isEmpty() ? "Merge failed (unknown error)" : errMsg);
                } else {
                    // Re-select so the inspector panel refreshes its animation list.
                    SelectionSet::getSingleton()->append(baseEntity);
                }
            }
        } else {
            QMessageBox::information(this, "Animation-only file",
                QString("'%1' contains no mesh geometry \u2014 %2 animation(s) were imported:%3\n\n"
                        "To use these animations: import the target mesh, select it, "
                        "then import this file again.")
                    .arg(displayName).arg(numAnims).arg(animList));
        }
    }
}

// LCOV_EXCL_START — opens QFileDialog
void MainWindow::on_actionOpen_Scene_triggered()
{
    SentryReporter::addBreadcrumb("ui.action", "Open scene file");

    QString fileName = QFileDialog::getOpenFileName(this, tr("Open Scene"),
                                                    "",
                                                    tr("Scene Files (*.scene.glb *.scene.gltf);;glTF Files (*.gltf *.glb);;All Files (*)"),
                                                    nullptr, QFileDialog::DontUseNativeDialog);
    if (fileName.isEmpty()) return;

    auto txn = SentryReporter::startTransaction("ui.import", "scene.import");
    try {
        MeshImporterExporter::sceneImporter(fileName);
    } catch (...) {
        SentryReporter::finishTransaction(txn);
        throw;
    }
    SentryReporter::finishTransaction(txn);
    addToRecentFiles(fileName);
}
// LCOV_EXCL_STOP

// LCOV_EXCL_START — opens QFileDialog
void MainWindow::on_actionSave_Scene_triggered()
{
    SentryReporter::addBreadcrumb("ui.action", "Save scene file");

    QString fileName = QFileDialog::getSaveFileName(this, tr("Save Scene"),
                                                    "scene.scene.glb",
                                                    tr("Scene glTF Binary (*.scene.glb);;Scene glTF (*.scene.gltf)"),
                                                    nullptr, QFileDialog::DontUseNativeDialog);
    if (fileName.isEmpty()) return;

    QProgressDialog progressDialog(tr("Saving scene..."), QString(), 0, 100, this);
    progressDialog.setWindowModality(Qt::WindowModal);
    progressDialog.setCancelButton(nullptr);
    progressDialog.setMinimumDuration(0);
    progressDialog.setValue(0);

    auto txn = SentryReporter::startTransaction("ui.export", "scene.export");
    try {
        int result = MeshImporterExporter::sceneExporter(fileName,
            [&progressDialog](int progress, const QString& status) {
                progressDialog.setLabelText(status);
                progressDialog.setValue(progress);
                QApplication::processEvents();
            });
        if (result != 0)
            QMessageBox::warning(this, tr("Save Scene"), tr("Failed to save scene."));
    } catch (...) {
        SentryReporter::finishTransaction(txn);
        throw;
    }
    SentryReporter::finishTransaction(txn);
}
// LCOV_EXCL_STOP

// LCOV_EXCL_START — opens QFileDialog
void MainWindow::on_actionExport_Selected_triggered()
{
    SentryReporter::addBreadcrumb("ui.action", "Export selected mesh");
    auto txn = SentryReporter::startTransaction("ui.export", "file.export");

    try {
        const auto* sel = SelectionSet::getSingleton();

        if(sel->hasNodes())
        {
            foreach(Ogre::SceneNode* node, sel->getNodesSelectionList())
            {
                QString exportedPath = MeshImporterExporter::exporter(node);
                if (!exportedPath.isEmpty())
                    addToRecentFiles(exportedPath);
            }
        }
        else if(sel->hasEntities())
        {
            foreach(Ogre::Entity* entity, sel->getEntitiesSelectionList())
            {
                auto* node = entity->getParentSceneNode();
                if (!node) continue;
                QString exportedPath = MeshImporterExporter::exporter(node);
                if (!exportedPath.isEmpty())
                    addToRecentFiles(exportedPath);
            }
        }
    } catch (...) {
        SentryReporter::finishTransaction(txn);
        throw;
    }
    SentryReporter::finishTransaction(txn);
}
// LCOV_EXCL_STOP

// LCOV_EXCL_START — opens QML window
void MainWindow::on_actionMaterial_Editor_triggered()
{
    try {
        // Force software rendering to avoid OpenGL conflicts with Ogre
        qputenv("QSG_RHI_BACKEND", "software");
        qputenv("QT_QUICK_BACKEND", "software");
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);

        // Create QML Application Engine for material list modal
        QQmlApplicationEngine* engine = new QQmlApplicationEngine(this);
        
        // Add QML import paths so the engine can find QtQuick.Controls etc.
        // This is needed when Qt libraries are bundled with the app.
        QString appDir = QCoreApplication::applicationDirPath();
        engine->addImportPath(appDir + "/qml");
        engine->addImportPath(QLibraryInfo::path(QLibraryInfo::QmlImportsPath));
        
        // Force software rendering on the engine
        engine->setProperty("_q_sg_renderloop", "basic");
        
        // Register QML types - must match registrations in main.cpp
        qmlRegisterSingletonType<MaterialEditorQML>("MaterialEditorQML", 1, 0, "MaterialEditorQML", 
            [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject * {
                Q_UNUSED(engine)
                Q_UNUSED(scriptEngine)
                return MaterialEditorQML::qmlInstance(engine, scriptEngine);
            });
        
        // Register LLMManager singleton for QML
        qmlRegisterSingletonType<LLMManager>("MaterialEditorQML", 1, 0, "LLMManager",
            [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject* {
                Q_UNUSED(engine)
                Q_UNUSED(scriptEngine)
                return LLMManager::qmlInstance(engine, scriptEngine);
            });

        // Register ModelDownloader singleton for QML
        qmlRegisterSingletonType<ModelDownloader>("MaterialEditorQML", 1, 0, "ModelDownloader",
            [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject* {
                Q_UNUSED(engine)
                Q_UNUSED(scriptEngine)
                return ModelDownloader::qmlInstance(engine, scriptEngine);
            });

        // Register QMLMaterialHighlighter for QML use
        qmlRegisterType<QMLMaterialHighlighter>("MaterialEditorQML", 1, 0, "MaterialHighlighter");
        
        // Load the QML material list modal
        QUrl qmlUrl("qrc:/MaterialEditorQML/MaterialListModal.qml");
        qDebug() << "Attempting to load QML Material List Modal from:" << qmlUrl.toString();
        
        // Connect to check for loading errors
        connect(engine, &QQmlApplicationEngine::objectCreated, this, [this, engine](QObject *obj, const QUrl &objUrl) {
            if (!obj) {
                qDebug() << "QML Material List Modal failed to load";
                engine->deleteLater();
                
                QMessageBox::critical(this, "QML Modal Error", 
                    "QML Material List Modal failed to load. Please check the QML files and try again.");
            } else {
                qDebug() << "QML Material List Modal loaded successfully";
                // Set window title
                if (auto window = qobject_cast<QQuickWindow*>(obj)) {
                    window->setTitle("Material List");
                }
            }
        });
        
        engine->load(qmlUrl);
        
    } catch (const std::exception& e) {
        qDebug() << "Exception in QML Material List Modal creation:" << e.what();
        QMessageBox::critical(this, "Material List Error", 
            QString("QML Material List Modal encountered an error: %1").arg(e.what()));
        
    } catch (...) {
        qDebug() << "Unknown exception in QML Material List Modal creation";
        QMessageBox::critical(this, "Material List Error",
            "QML Material List Modal encountered an unknown error.");
    }
}
// LCOV_EXCL_STOP

void MainWindow::updateMergeAnimationsButton()
{
    auto entities = SelectionSet::getSingleton()->getResolvedEntities();

    // Filter to entities with skeletons
    QList<Ogre::Entity*> skelEntities;
    for (Ogre::Entity* e : entities)
    {
        if (e && e->hasSkeleton())
            skelEntities.append(e);
    }

    if (skelEntities.size() < 2)
    {
        ui->actionMerge_Animations->setEnabled(false);
        return;
    }

    // Check all pairs are compatible
    Ogre::SkeletonPtr baseSkel = skelEntities.first()->getMesh()->getSkeleton();
    for (int i = 1; i < skelEntities.size(); ++i)
    {
        Ogre::SkeletonPtr otherSkel = skelEntities[i]->getMesh()->getSkeleton();
        if (!AnimationMerger::areSkeletonsCompatible(baseSkel, otherSkel))
        {
            ui->actionMerge_Animations->setEnabled(false);
            return;
        }
    }

    ui->actionMerge_Animations->setEnabled(true);
}

// LCOV_EXCL_START — complex dialog with entity merging
void MainWindow::on_actionMerge_Animations_triggered()
{
    auto entities = SelectionSet::getSingleton()->getResolvedEntities();

    // Filter to entities with skeletons
    QList<Ogre::Entity*> skelEntities;
    for (Ogre::Entity* e : entities)
    {
        if (e && e->hasSkeleton())
            skelEntities.append(e);
    }

    if (skelEntities.size() < 2)
    {
        QMessageBox::warning(this, tr("Merge Animations"),
            tr("Select at least 2 objects with skeletons to merge."));
        return;
    }

    Ogre::Entity* baseEntity = skelEntities.first();
    QString errorMsg;
    Ogre::Entity* merged = nullptr;

    try {
        merged = AnimationMerger::mergeAnimations(baseEntity, skelEntities, errorMsg);
    } catch (const Ogre::Exception& e) {
        errorMsg = QString("Ogre error: %1").arg(e.getFullDescription().c_str());
    } catch (const std::exception& e) {
        errorMsg = QString("Error: %1").arg(e.what());
    }

    if (!merged)
    {
        QMessageBox::warning(this, tr("Merge Animations"), errorMsg);
        return;
    }

    // Clear selection BEFORE destroying nodes to avoid dangling references
    SelectionSet::getSingleton()->clear();

    // Collect nodes to destroy (separate from iteration to avoid issues)
    QList<Ogre::SceneNode*> nodesToDestroy;
    for (int i = 1; i < skelEntities.size(); ++i)
    {
        Ogre::Entity* e = skelEntities[i];
        if (auto* node = e->getParentSceneNode())
            nodesToDestroy.append(node);
    }

    for (Ogre::SceneNode* node : nodesToDestroy)
        Manager::getSingleton()->destroySceneNode(node);

    // Select the merged entity
    SelectionSet::getSingleton()->append(baseEntity);

    int animCount = 0;
    if (auto* set = baseEntity->getAllAnimationStates())
        animCount = static_cast<int>(set->getAnimationStates().size());

    QMessageBox::information(this, tr("Merge Animations"),
        tr("Successfully merged animations. The result has %1 animation(s).")
            .arg(animCount));
}
// LCOV_EXCL_STOP

void MainWindow::on_actionAbout_triggered()
{
    About *m = new About(this);
    m->show();
}

void MainWindow::on_actionObjects_Toolbar_toggled(bool arg1)
{    ui->objectsToolbar->setVisible(arg1);  }

void MainWindow::on_actionTools_Toolbar_toggled(bool arg1)
{    ui->toolToolbar->setVisible(arg1); }

void MainWindow::on_actionView_Toolbar_toggled(bool arg1)
{    ui->viewToolbar->setVisible(arg1); }

void MainWindow::on_actionMeshEditor_toggled(bool arg1)
{    ui->meshEditorWidget->setVisible(arg1);    }

// LCOV_EXCL_START — color dialog
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
// LCOV_EXCL_STOP

void MainWindow::setTransformState(TransformOperator::TransformState newState)
{
    ui->actionSelect_Object->setChecked(newState == TransformOperator::TS_SELECT);
    ui->actionTranslate_Object->setChecked(newState == TransformOperator::TS_TRANSLATE);
    ui->actionRotate_Object->setChecked(newState == TransformOperator::TS_ROTATE);
    ui->actionScale_Object->setChecked(newState == TransformOperator::TS_SCALE);

    TransformOperator::getSingleton()->onTransformStateChange(newState);
}

// LCOV_EXCL_START — creates OgreWidget, requires display
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
    if (m_meshInfoOverlay)
        connect(pOgreViewport->getOgreWidget(), &OgreWidget::focusOnWidget, m_meshInfoOverlay, &MeshInfoOverlay::setActiveWidget);
    if (m_viewCubeController)
        connect(pOgreViewport->getOgreWidget(), &OgreWidget::focusOnWidget, this, [this](OgreWidget* w) {
            m_viewCubeController->setActiveWidget(w);
        });

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
    ui->action2x2_Grid->blockSignals(true);
    ui->actionSingle->setChecked(false);
    ui->action1x1_Side_by_Side->setChecked(false);
    ui->action1x1_Upper_and_Lower->setChecked(false);
    ui->action2x2_Grid->setChecked(false);
    ui->actionSingle->blockSignals(false);
    ui->action1x1_Side_by_Side->blockSignals(false);
    ui->action1x1_Upper_and_Lower->blockSignals(false);
    ui->action2x2_Grid->blockSignals(false);
}

void MainWindow::onWidgetClosing(EditorViewport* const& widget)
{
    // Artificial MUTEX !!! don't know if required
    // Safety check: don't access timer if MainWindow is being destroyed
    if(m_pTimer)
    {
        m_pTimer->stop();
    }

    bool result = mDockWidgetList.removeOne(widget);

    if(result)
    {
        // Disconnect signals before deletion to prevent callbacks during destruction
        disconnect(widget, nullptr, this, nullptr);
        disconnect(widget->getOgreWidget(), nullptr, this, nullptr);
        // Use deleteLater to avoid use-after-free: closeEvent is still on the call stack
        widget->deleteLater();
    }
    else
        qDebug()<<"Unable to remove viewport "<<widget->getIndex();

    // Safety check: don't restart timer if MainWindow is being destroyed
    if(m_pTimer)
    {
        m_pTimer->start(0);
    }
}
// LCOV_EXCL_STOP

// LCOV_EXCL_START — viewport layout changes require OgreWidget
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
        ui->action2x2_Grid->setChecked(false);
    } else { //Doesn't allow unchecking
        ui->actionSingle->setChecked(   !ui->action1x1_Side_by_Side->isChecked() &&
                                        !ui->action1x1_Upper_and_Lower->isChecked() &&
                                        !ui->action2x2_Grid->isChecked());
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

        // Remove and re-add to fully reset the internal splitter layout
        removeDockWidget(mDockWidgetList.first());
        removeDockWidget(mDockWidgetList.last());
        addDockWidget(Qt::LeftDockWidgetArea, mDockWidgetList.first());
        addDockWidget(Qt::LeftDockWidgetArea, mDockWidgetList.last());
        mDockWidgetList.first()->show();
        mDockWidgetList.last()->show();
        splitDockWidget(mDockWidgetList.first(),mDockWidgetList.last(),Qt::Horizontal);

        // Defer resize to equalize after layout is processed
        QTimer::singleShot(0, this, [this]() {
            if (mDockWidgetList.size() >= 2) {
                int halfWidth = width() / 2;
                resizeDocks({mDockWidgetList.first(), mDockWidgetList.last()}, {halfWidth, halfWidth}, Qt::Horizontal);
            }
        });

        ui->actionSingle->setChecked(false);
        ui->action1x1_Side_by_Side->setChecked(true);
        ui->action1x1_Upper_and_Lower->setChecked(false);
        ui->action2x2_Grid->setChecked(false);
    } else { //Doesn't allow unchecking
        ui->action1x1_Side_by_Side->setChecked( !ui->actionSingle->isChecked() &&
                                                !ui->action1x1_Upper_and_Lower->isChecked() &&
                                                !ui->action2x2_Grid->isChecked());
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

        // Remove and re-add to fully reset the internal splitter layout
        removeDockWidget(mDockWidgetList.first());
        removeDockWidget(mDockWidgetList.last());
        addDockWidget(Qt::LeftDockWidgetArea, mDockWidgetList.first());
        addDockWidget(Qt::LeftDockWidgetArea, mDockWidgetList.last());
        mDockWidgetList.first()->show();
        mDockWidgetList.last()->show();
        splitDockWidget(mDockWidgetList.first(),mDockWidgetList.last(),Qt::Vertical);

        // Defer resize to equalize after layout is processed
        QTimer::singleShot(0, this, [this]() {
            if (mDockWidgetList.size() >= 2) {
                int halfHeight = height() / 2;
                resizeDocks({mDockWidgetList.first(), mDockWidgetList.last()}, {halfHeight, halfHeight}, Qt::Vertical);
            }
        });

        ui->actionSingle->setChecked(false);
        ui->action1x1_Side_by_Side->setChecked(false);
        ui->action1x1_Upper_and_Lower->setChecked(true);
        ui->action2x2_Grid->setChecked(false);
    } else { //Doesn't allow unchecking
        ui->action1x1_Upper_and_Lower->setChecked(  !ui->actionSingle->isChecked() &&
                                                    !ui->action1x1_Side_by_Side->isChecked() &&
                                                    !ui->action2x2_Grid->isChecked());
    }
}

void MainWindow::on_action2x2_Grid_toggled(bool arg1)
{
    if(arg1)
    {
        while(mDockWidgetList.size()<4)
        {
            createEditorViewport();
        }
        while(mDockWidgetList.size()>4)
        {
            mDockWidgetList.last()->close();
        }

        // Arrange as 2x2 grid:
        // Top row: viewport[0] | viewport[1]
        splitDockWidget(mDockWidgetList[0], mDockWidgetList[1], Qt::Horizontal);
        // Bottom-left under viewport[0]
        splitDockWidget(mDockWidgetList[0], mDockWidgetList[2], Qt::Vertical);
        // Bottom-right under viewport[1]
        splitDockWidget(mDockWidgetList[1], mDockWidgetList[3], Qt::Vertical);

        // Defer resize to after Qt processes the split layout
        QTimer::singleShot(0, this, [this]() {
            int halfWidth = width() / 2;
            int halfHeight = height() / 2;
            resizeDocks({mDockWidgetList[0], mDockWidgetList[1]}, {halfWidth, halfWidth}, Qt::Horizontal);
            resizeDocks({mDockWidgetList[0], mDockWidgetList[2]}, {halfHeight, halfHeight}, Qt::Vertical);
            resizeDocks({mDockWidgetList[1], mDockWidgetList[3]}, {halfHeight, halfHeight}, Qt::Vertical);
        });

        ui->actionSingle->setChecked(false);
        ui->action1x1_Side_by_Side->setChecked(false);
        ui->action1x1_Upper_and_Lower->setChecked(false);
        ui->action2x2_Grid->setChecked(true);
    } else { //Doesn't allow unchecking
        ui->action2x2_Grid->setChecked( !ui->actionSingle->isChecked() &&
                                        !ui->action1x1_Side_by_Side->isChecked() &&
                                        !ui->action1x1_Upper_and_Lower->isChecked());
    }
}
// LCOV_EXCL_STOP

// LCOV_EXCL_START — file dialog
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
// LCOV_EXCL_STOP

// LCOV_EXCL_START — color dialog
void MainWindow::on_actionChange_Ambient_Light_triggered()
{
    Ogre::ColourValue c = Manager::getSingleton()->getSceneMgr()->getAmbientLight();
    ambientLightColorDialog->setCurrentColor(QColor(c.r*255,c.g*255,c.b*255,c.a*255));
    ambientLightColorDialog->show();
}
// LCOV_EXCL_STOP

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

// LCOV_EXCL_START — color dialog interaction
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
// LCOV_EXCL_STOP

// LCOV_EXCL_START — palette modification
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
// LCOV_EXCL_STOP

// LCOV_EXCL_START — network request
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
// LCOV_EXCL_STOP

// LCOV_EXCL_START — dialogs and server lifecycle
void MainWindow::showAIModelSettings()
{
    LLMSettingsWidget* settingsWidget = new LLMSettingsWidget(this);
    settingsWidget->setAttribute(Qt::WA_DeleteOnClose);
    settingsWidget->exec();
}

void MainWindow::showMCPSettings()
{
    bool running = m_mcpServer && m_mcpServer->isHttpRunning();
    int port = m_mcpServer ? m_mcpServer->httpPort()
                           : QSettings().value("MCP/port", 8080).toInt();

    MCPSettingsDialog dialog(running, port, this);
    dialog.startCallback = [this](int p) { return startMCPServer(p); };
    dialog.stopCallback  = [this]()      { stopMCPServer(); };
    dialog.exec();
}

bool MainWindow::startMCPServer(int port)
{
    if (m_mcpServer && m_mcpServer->isHttpRunning())
        return true;

    if (!m_mcpServer) {
        m_mcpServer = new MCPServer(this);
        m_mcpServer->setMainWindow(this);
        AIChatManager::instance()->setMcpServer(m_mcpServer);
    }

    bool ok = m_mcpServer->startHttp(port);
    if (ok) {
        QSettings settings;
        settings.setValue("MCP/enabled", true);
        settings.setValue("MCP/port", port);
    }
    return ok;
}

void MainWindow::stopMCPServer()
{
    if (m_mcpServer) {
        m_mcpServer->stopHttp();
    }
    QSettings settings;
    settings.setValue("MCP/enabled", false);
}
// LCOV_EXCL_STOP

void MainWindow::setMCPServer(MCPServer* server)
{
    if (m_mcpServer && m_mcpServer != server) {
        m_mcpServer->stop();
        delete m_mcpServer;
    }
    m_mcpServer = server;
    AIChatManager::instance()->setMcpServer(m_mcpServer);
}

void MainWindow::addToRecentFiles(const QString& filePath)
{
    QSettings settings;
    QStringList files = settings.value("RecentFiles/files").toStringList();
    files.removeAll(filePath);
    files.prepend(filePath);
    while (files.size() > 10)
        files.removeLast();
    settings.setValue("RecentFiles/files", files);
    updateRecentFilesMenu();
}

void MainWindow::updateRecentFilesMenu()
{
    m_recentFilesMenu->clear();

    QSettings settings;
    if (QStringList files = settings.value("RecentFiles/files").toStringList(); files.isEmpty()) {
        auto* noFilesAction = m_recentFilesMenu->addAction(tr("(No Recent Files)"));
        noFilesAction->setEnabled(false);
    } else {
        for (const QString& filePath : files) {
            QFileInfo fi(filePath);
            auto* action = m_recentFilesMenu->addAction(fi.fileName());
            action->setData(filePath);
            action->setToolTip(filePath);
            connect(action, &QAction::triggered, this, &MainWindow::openRecentFile);
        }
    }

    m_recentFilesMenu->addSeparator();
    const auto* clearAction = m_recentFilesMenu->addAction(tr("Clear Recent Files"));
    connect(clearAction, &QAction::triggered, this, [this]() {
        QSettings settings;
        settings.remove("RecentFiles/files");
        updateRecentFilesMenu();
    });
}

void MainWindow::openRecentFile()
{
    const auto* action = qobject_cast<QAction*>(sender());
    if (!action)
        return;

    QString filePath = action->data().toString();
    if (QFileInfo::exists(filePath)) {
        addToRecentFiles(filePath);
        if (filePath.endsWith(".scene.glb") || filePath.endsWith(".scene.gltf"))
            MeshImporterExporter::sceneImporter(filePath);
        else
            mUriList.append(filePath);
    } else {
        QMessageBox::warning(this, tr("File Not Found"),
            tr("The file \"%1\" no longer exists.").arg(filePath));
        QSettings settings;
        QStringList files = settings.value("RecentFiles/files").toStringList();
        files.removeAll(filePath);
        settings.setValue("RecentFiles/files", files);
        updateRecentFilesMenu();
    }
}

