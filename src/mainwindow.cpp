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
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
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
#include "SubEntityHighlight.h"
#include "SpaceCamera.h"
#include "ViewCube/ViewCubeController.h"
#include "LLMManager.h"
#include "QMLMaterialHighlighter.h"
#include "ModelDownloader.h"
#include "UndoManager.h"
#include "commands/TransformCommands.h"
#include "PropertiesPanelController.h"
#include "MeshLodController.h"
#include "MeshValidator.h"
#include "MaterialPresetLibrary.h"
#include "MaterialPreviewRenderer.h"
#include "AIChatManager.h"
#include "WelcomeScreenController.h"
#include "AssetBrowserController.h"
#include "EditModeController.h"
#include <QDockWidget>
#include <QQuickWidget>
#include <QQmlContext>
#include <QToolButton>
#include <QMenu>
#include <QWidgetAction>
#include <QSlider>
#include <QColorDialog>
#include <QSignalBlocker>
#include <QGridLayout>

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

    // Edit Mode indicator in status bar
    m_editModeLabel = new QLabel("Object Mode", this);
    m_editModeLabel->setStyleSheet("QLabel { font-weight: bold; padding: 2px 8px; }");
    statusBar()->addPermanentWidget(m_editModeLabel);
    connect(EditModeController::instance(), &EditModeController::editModeChanged,
            this, &MainWindow::updateEditModeIndicator);
    updateEditModeIndicator();

    // Surface edit-mode hint messages (e.g. "Loop cut needs a quad mesh")
    // via a dedicated permanent label. The status bar's main message slot
    // is rewritten every frame by frameEnded(), so showMessage() lasted
    // ~16ms in practice. This label persists for 5s via QTimer::singleShot.
    m_editHintLabel = new QLabel(this);
    m_editHintLabel->setStyleSheet(
        "QLabel { color: #ffaa33; font-style: italic; padding: 2px 8px; }");
    m_editHintLabel->setVisible(false);
    statusBar()->addPermanentWidget(m_editHintLabel);
    connect(EditModeController::instance(), &EditModeController::editHintMessage,
            this, [this](const QString& msg) {
                m_editHintLabel->setText(msg);
                m_editHintLabel->setVisible(true);
                QTimer::singleShot(5000, m_editHintLabel,
                    [this]() { m_editHintLabel->setVisible(false); });
            });

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
    // In tests, other suites may have already destroyed Manager (and therefore
    // Ogre resources). In that case, destroying Ogre-backed singletons can
    // crash due to dangling SceneManager pointers — skip teardown and let the
    // process exit cleanly.
    Manager* manager = Manager::getSingletonPtr();
    if (manager) {
        EditModeController::kill();
        SubEntityHighlight::kill();
        AnimationControlController::kill();
        MeshLodController::kill();
        MeshValidator::kill();
        MaterialPresetLibrary::kill();
        MaterialPreviewRenderer::kill();
        AIChatManager::kill();

        // Only destroy Manager if it still exists and belongs to this MainWindow
        if (manager->getMainWindow() == this)
            Manager::kill();
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

    setTransformState(TransformOperator::TS_SELECT);
    connect(ui->actionSelect_Object, &QAction::triggered, this, [this]{setTransformState(TransformOperator::TS_SELECT);});
    connect(ui->actionTranslate_Object, &QAction::triggered, this, [this]{setTransformState(TransformOperator::TS_TRANSLATE);});
    connect(ui->actionRotate_Object, &QAction::triggered, this, [this]{setTransformState(TransformOperator::TS_ROTATE);});
    connect(ui->actionScale_Object, &QAction::triggered, this, [this]{setTransformState(TransformOperator::TS_SCALE);});
    connect(ui->actionRemove_Object, &QAction::triggered, this, []{
        if (!EditModeController::instance()->isEditModeActive())
            TransformOperator::getSingleton()->removeSelected();
    });
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

    // Duplicate
    connect(ui->actionDuplicate, &QAction::triggered, this, &MainWindow::duplicateSelected);

    // Group / Ungroup
    connect(ui->actionGroup, &QAction::triggered, this, &MainWindow::groupSelected);
    connect(ui->actionUngroup, &QAction::triggered, this, &MainWindow::ungroupSelected);

    // Enable/disable group actions based on selection
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged, this, [this]() {
        auto* sel = SelectionSet::getSingleton();
        int nodeCount = sel->getNodesCount();
        ui->actionGroup->setEnabled(nodeCount >= 2);
        bool canUngroup = (nodeCount == 1) && Manager::getSingleton()->isGroupNode(
            sel->getNodesSelectionList().first());
        ui->actionUngroup->setEnabled(canUngroup);
    });

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
        qmlRegisterSingletonType<WelcomeScreenController>("WelcomeScreen", 1, 0, "WelcomeScreenController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return WelcomeScreenController::qmlInstance(engine, nullptr);
            });
        qmlRegisterSingletonType<AssetBrowserController>("AssetBrowser", 1, 0, "AssetBrowserController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return AssetBrowserController::qmlInstance(engine, nullptr);
            });
        qmlRegisterSingletonType<EditModeController>("PropertiesPanel", 1, 0, "EditModeController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return EditModeController::qmlInstance(engine, nullptr);
            });

        m_propertiesPanel->setSource(QUrl("qrc:/PropertiesPanel/PropertiesPanel.qml"));

        // Force QQuickWidget repaint when snap settings change — QQuickWidget
        // inside a QDockWidget doesn't repaint on internal QML property changes.
        // QWidget::update() alone isn't enough; must also request a new frame
        // from the QML scene graph via quickWindow().
        connect(TransformOperator::getSingleton(), &TransformOperator::snapSettingsChanged,
                this, [this]() {
            if (m_propertiesPanel) {
                m_propertiesPanel->update();
                if (m_propertiesPanel->quickWindow())
                    m_propertiesPanel->quickWindow()->requestUpdate();
            }
        });

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

        // When focus lands on the dock container (not the QQuickWidget inside),
        // forward it to the QQuickWidget. This fixes the macOS issue where
        // clicking the chat input after switching back from another app requires
        // two clicks — the first activates the window but focus stays on the dock.
        connect(qApp, &QApplication::focusChanged, this, [chatWidget, this](QWidget*, QWidget* now) {
            if (!now || !m_chatDock || !m_chatDock->isVisible()) return;
            if (now == m_chatDock || (now->parentWidget() && now->parentWidget() == m_chatDock)) {
                if (now != chatWidget)
                    QTimer::singleShot(0, chatWidget, [chatWidget]() { chatWidget->setFocus(); });
            }
        });
    }

    // Asset Browser dock
    {
        auto* assetBrowserWidget = new QQuickWidget();
        assetBrowserWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
        assetBrowserWidget->setMinimumWidth(250);
        assetBrowserWidget->setMinimumHeight(200);
        assetBrowserWidget->setFocusPolicy(Qt::StrongFocus);
        assetBrowserWidget->setSource(QUrl("qrc:/AssetBrowser/AssetBrowser.qml"));
        m_assetBrowserDock = new QDockWidget(tr("Asset Browser"), this);
        m_assetBrowserDock->setWidget(assetBrowserWidget);
        m_assetBrowserDock->setObjectName("AssetBrowserDock");
        addDockWidget(Qt::BottomDockWidgetArea, m_assetBrowserDock);
        m_assetBrowserDock->hide();

        // Connect Browse button — open a native directory picker from MainWindow
        // (QFileDialog needs a proper parent widget on macOS)
        auto* abController = AssetBrowserController::instance();
        connect(abController, &AssetBrowserController::importMeshRequested, this, [this](const QStringList& paths) {
            SentryReporter::addBreadcrumb("ui.action", "Asset Browser: import mesh");
            importMeshs(paths);
        });
    }

    // Welcome Screen overlay — shown on first launch or when user hasn't opted out
    {
        m_welcomeController = WelcomeScreenController::instance();
        m_welcomeController->setMainWindow(this);

        m_welcomeScreen = new QQuickWidget(this);
        m_welcomeScreen->setResizeMode(QQuickWidget::SizeRootObjectToView);
        m_welcomeScreen->setAttribute(Qt::WA_TranslucentBackground);
        m_welcomeScreen->setClearColor(Qt::transparent);
        m_welcomeScreen->setSource(QUrl("qrc:/WelcomeScreen/WelcomeScreen.qml"));
        m_welcomeScreen->setFocusPolicy(Qt::StrongFocus);
        m_welcomeScreen->raise();

        // Connect controller signals to MainWindow actions
        connect(m_welcomeController, &WelcomeScreenController::requestOpenFile,
                this, [this](const QString& path) {
            if (QFileInfo::exists(path)) {
                addToRecentFiles(path);
                if (path.endsWith(".scene.glb") || path.endsWith(".scene.gltf"))
                    MeshImporterExporter::sceneImporter(path);
                else
                    mUriList.append(path);
            }
        });
        connect(m_welcomeController, &WelcomeScreenController::requestOpenFileDialog,
                this, &MainWindow::on_actionImport_triggered);
        connect(m_welcomeController, &WelcomeScreenController::requestNewScene,
                this, [this]() {
            Manager::getSingleton()->CreateEmptyScene();
        });

        // Show/hide the overlay widget when controller visibility changes
        connect(m_welcomeController, &WelcomeScreenController::visibleChanged,
                this, [this]() {
            if (m_welcomeController->isVisible()) {
                showWelcomeScreen();
            } else {
                hideWelcomeScreen();
            }
        });

        // Welcome screen is now a standalone dialog shown before MainWindow (in main.cpp).
        // The QML overlay is kept for programmatic use but not shown on startup.
        {
            m_welcomeScreen->hide();
        }
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
    aiChatButton->setObjectName("aiChatToolbarButton");
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

    // Topology tools — toolbar shortcuts for Extrude / Bevel. They
    // delegate to the same EditModeController actions the Inspector
    // buttons used to trigger, and enable/disable themselves based on
    // the current edit-mode state so the Inspector can be slimmed down.
    //
    // Styling: matches the primitive icons' light-green gradient look.
    // Disabled state desaturates to a subtle gray so the buttons don't
    // shout when they're not actionable (e.g., Extrude in vertex mode).
    auto* editCtrlForTopo = EditModeController::instance();

    // Shared stylesheet for the glyph buttons. Uses a CSS linear-gradient
    // on the text color (via color: qlineargradient isn't supported in
    // widgets — Qt clips to a single color — so we pick the midtone
    // green of the icon family for the enabled state and a muted gray
    // for the disabled state).
    const char* topoBtnStyle = R"(
        QToolButton {
            color: #7bbd2a;
            border: none;
            padding: 2px 4px;
        }
        QToolButton:checked {
            color: #9adc4a;
            background-color: rgba(122, 189, 42, 0.18);
            border: 1px solid rgba(122, 189, 42, 0.45);
            border-radius: 3px;
        }
        QToolButton:hover:enabled {
            color: #9adc4a;
        }
        QToolButton:pressed:enabled {
            color: #5a9a1a;
        }
        QToolButton:disabled {
            color: #b8b8b8;
        }
    )";

    // Extrude: face mode only.
    auto extrudeButton = new QToolButton(ui->objectsToolbar);
    extrudeButton->setText("\u2B06");  // ⬆ (extrude = push outward)
    const QString extrudeShortcutLabel =
#ifdef Q_OS_MACOS
        QStringLiteral("Cmd+E");
#else
        QStringLiteral("Ctrl+E");
#endif
    extrudeButton->setToolTip(tr("Extrude selected faces (%1)").arg(extrudeShortcutLabel));
    QFont topoFont = extrudeButton->font();
    topoFont.setPixelSize(16);
    topoFont.setBold(true);
    extrudeButton->setFont(topoFont);
    extrudeButton->setStyleSheet(topoBtnStyle);
    // On successful extrude, auto-switch to the Translate tool so the
    // user can immediately move the freshly-extruded geometry.
    connect(extrudeButton, &QToolButton::clicked, this, [this]() {
        SentryReporter::addBreadcrumb("ui.action", "Toolbar: Extrude");
        auto* c = EditModeController::instance();
        if (c->extrudeSelection()) {
            setTransformState(TransformOperator::TS_TRANSLATE);
        }
    });
    // QToolBar::addWidget returns the QAction that wraps the widget.
    // Hiding the widget directly doesn't affect the toolbar's layout —
    // we have to toggle the action's visibility instead.
    QAction* extrudeAction = ui->objectsToolbar->addWidget(extrudeButton);

    // Bevel: edge OR vertex mode.
    auto bevelButton = new QToolButton(ui->objectsToolbar);
    bevelButton->setText("\u25E2");  // ◢ (cut corner)
    const QString bevelShortcutLabel =
#ifdef Q_OS_MACOS
        QStringLiteral("Cmd+B");
#else
        QStringLiteral("Ctrl+B");
#endif
    bevelButton->setToolTip(tr("Bevel selected edges / vertices (%1)").arg(bevelShortcutLabel));
    bevelButton->setFont(topoFont);
    bevelButton->setStyleSheet(topoBtnStyle);
    connect(bevelButton, &QToolButton::clicked, this, []() {
        SentryReporter::addBreadcrumb("ui.action", "Toolbar: Bevel");
        EditModeController::instance()->bevelSelection();
    });
    QAction* bevelAction = ui->objectsToolbar->addWidget(bevelButton);

    // Knife: opens a multi-point cut session. Available in edit mode
    // regardless of selection component (vertex/edge/face), because
    // the knife's own hit-test snaps to geometry.
    auto knifeButton = new QToolButton(ui->objectsToolbar);
    knifeButton->setText(QStringLiteral("\u2702"));  // ✂ scissors
    knifeButton->setToolTip(tr("Knife — place cut points, Enter to commit, Esc to cancel (K)"));
    knifeButton->setFont(topoFont);
    knifeButton->setStyleSheet(topoBtnStyle);
    connect(knifeButton, &QToolButton::clicked, this, []() {
        SentryReporter::addBreadcrumb("ui.action", "Toolbar: Knife");
        auto* c = EditModeController::instance();
        // Pressing the button while a session is already open commits
        // it — matches the bevel button pattern (click = open / commit).
        if (c->knifeSessionActive()) c->commitKnife();
        else                         c->beginKnife();
    });
    QAction* knifeAction = ui->objectsToolbar->addWidget(knifeButton);

    // Merge: vertex-mode-only collapse of the current selection. Drops a
    // small popup so the user can pick the survivor target (Center / First
    // / Last / By Distance) without having to memorise a four-key chord.
    auto mergeButton = new QToolButton(ui->objectsToolbar);
    mergeButton->setText(QStringLiteral("\u2A00"));   // ⨀ circled-dot — fuse
    mergeButton->setToolTip(tr("Merge vertices… — collapse selection (M)"));
    mergeButton->setFont(topoFont);
    mergeButton->setStyleSheet(topoBtnStyle);
    mergeButton->setPopupMode(QToolButton::InstantPopup);

    auto mergeMenu = new QMenu(mergeButton);
    auto* actAtCenter   = mergeMenu->addAction(tr("At Center"));
    auto* actAtFirst    = mergeMenu->addAction(tr("At First"));
    auto* actAtLast     = mergeMenu->addAction(tr("At Last"));
    mergeMenu->addSeparator();
    auto* actByDistance = mergeMenu->addAction(tr("By Distance"));
    mergeButton->setMenu(mergeMenu);

    auto runMerge = [](int (EditModeController::*op)(), const char* label) {
        SentryReporter::addBreadcrumb("ui.action",
            QString("Toolbar: Merge %1").arg(label));
        auto* c = EditModeController::instance();
        const int removed = (c->*op)();
        Q_UNUSED(removed); // status reporting handled by Sentry breadcrumb for now
    };
    connect(actAtCenter, &QAction::triggered, this, [runMerge]() {
        runMerge(&EditModeController::mergeAtCenter, "At Center");
    });
    connect(actAtFirst, &QAction::triggered, this, [runMerge]() {
        runMerge(&EditModeController::mergeAtFirst, "At First");
    });
    connect(actAtLast, &QAction::triggered, this, [runMerge]() {
        runMerge(&EditModeController::mergeAtLast, "At Last");
    });
    connect(actByDistance, &QAction::triggered, this, []() {
        SentryReporter::addBreadcrumb("ui.action", "Toolbar: Merge By Distance");
        EditModeController::instance()->mergeByDistance(1e-4f);
    });
    QAction* mergeAction = ui->objectsToolbar->addWidget(mergeButton);

    // Delete / Dissolve: works in any selection mode. The dropdown lets
    // the user pick "Delete" (remove element + adjacent geometry) or
    // "Dissolve" (remove element while keeping the surrounding region
    // watertight). Bound to X (delete) and Ctrl+X / Cmd+X (dissolve).
    auto deleteButton = new QToolButton(ui->objectsToolbar);
    deleteButton->setText(QStringLiteral("\u2715"));  // ✕ (cross — destructive)
    const QString deleteShortcutLabel =
#ifdef Q_OS_MACOS
        QStringLiteral("X / Cmd+X");
#else
        QStringLiteral("X / Ctrl+X");
#endif
    deleteButton->setToolTip(tr("Delete / Dissolve selection (%1)").arg(deleteShortcutLabel));
    deleteButton->setFont(topoFont);
    deleteButton->setStyleSheet(topoBtnStyle);
    deleteButton->setPopupMode(QToolButton::InstantPopup);

    auto deleteMenu = new QMenu(deleteButton);
    auto* actDelete   = deleteMenu->addAction(tr("Delete"));
    auto* actDissolve = deleteMenu->addAction(tr("Dissolve"));
    deleteButton->setMenu(deleteMenu);

    connect(actDelete, &QAction::triggered, this, []() {
        SentryReporter::addBreadcrumb("ui.action", "Toolbar: Delete");
        EditModeController::instance()->deleteSelection();
    });
    connect(actDissolve, &QAction::triggered, this, []() {
        SentryReporter::addBreadcrumb("ui.action", "Toolbar: Dissolve");
        EditModeController::instance()->dissolveSelection();
    });
    QAction* deleteAction = ui->objectsToolbar->addWidget(deleteButton);

    // Subdivide: dropdown with two modes.
    //  - Standard: 1-to-4 triangle split on the selected faces/edges
    //    (what was always there).
    //  - Catmull-Clark: whole-mesh subdivide-surface step. Always
    //    produces quads regardless of input topology.
    auto subdivideButton = new QToolButton(ui->objectsToolbar);
    subdivideButton->setText(QStringLiteral("\u229E"));  // ⊞ box-plus, evokes a 4-cell split
    subdivideButton->setToolTip(tr("Subdivide… (Standard / Catmull-Clark)"));
    subdivideButton->setFont(topoFont);
    subdivideButton->setStyleSheet(topoBtnStyle);
    subdivideButton->setPopupMode(QToolButton::InstantPopup);
    auto subdivideMenu = new QMenu(subdivideButton);
    auto* actSubStandard = subdivideMenu->addAction(tr("Standard (1-to-4 split, selected faces)"));
    auto* actSubCC = subdivideMenu->addAction(tr("Catmull-Clark (whole mesh, smoothed quads)"));
    subdivideButton->setMenu(subdivideMenu);

    connect(actSubStandard, &QAction::triggered, this, []() {
        SentryReporter::addBreadcrumb("ui.action", "Toolbar: Subdivide (standard)");
        EditModeController::instance()->subdivideSelection();
    });
    connect(actSubCC, &QAction::triggered, this, []() {
        SentryReporter::addBreadcrumb("ui.action", "Toolbar: Subdivide (Catmull-Clark)");
        EditModeController::instance()->subdivideCatmullClarkAll();
    });
    QAction* subdivideAction = ui->objectsToolbar->addWidget(subdivideButton);

    // Fill: vertex mode (3-4+ verts → triangle / fan) or edge mode (closed
    // boundary loop → fan-triangulated cap, useful for capping holes).
    auto fillButton = new QToolButton(ui->objectsToolbar);
    fillButton->setText(QStringLiteral("\u25C6"));  // ◆ filled diamond — "fill"
    fillButton->setToolTip(tr("Fill selected vertices / edge loop (F)"));
    fillButton->setFont(topoFont);
    fillButton->setStyleSheet(topoBtnStyle);
    connect(fillButton, &QToolButton::clicked, this, []() {
        SentryReporter::addBreadcrumb("ui.action", "Toolbar: Fill");
        EditModeController::instance()->fillSelection();
    });
    QAction* fillAction = ui->objectsToolbar->addWidget(fillButton);

    // Loop cut: edge mode only — pick one edge, the op walks the
    // perpendicular ring of quads and bisects each one.
    auto loopCutButton = new QToolButton(ui->objectsToolbar);
    loopCutButton->setText(QStringLiteral("\u2551"));  // ‖ double vertical line — "loop"
    loopCutButton->setToolTip(tr("Loop Cut (Ctrl+R) — bisect quads perpendicular to the selected edge"));
    loopCutButton->setFont(topoFont);
    loopCutButton->setStyleSheet(topoBtnStyle);
    connect(loopCutButton, &QToolButton::clicked, this, []() {
        SentryReporter::addBreadcrumb("ui.action", "Toolbar: Loop Cut");
        EditModeController::instance()->loopCutSelection();
    });
    QAction* loopCutAction = ui->objectsToolbar->addWidget(loopCutButton);

    // Convert to Quads: walks the mesh and merges coplanar adjacent
    // triangle pairs into n-gon quads. Useful when an imported tri
    // mesh blocks loop cut / n-gon-aware bevel.
    auto convertToQuadsButton = new QToolButton(ui->objectsToolbar);
    convertToQuadsButton->setText(QStringLiteral("\u25A6"));  // ▦ — "tessellated/quad grid"
    convertToQuadsButton->setToolTip(tr("Convert to Quads — merge coplanar triangle pairs into quads"));
    convertToQuadsButton->setFont(topoFont);
    convertToQuadsButton->setStyleSheet(topoBtnStyle);
    connect(convertToQuadsButton, &QToolButton::clicked, this, []() {
        SentryReporter::addBreadcrumb("ui.action", "Toolbar: Convert to Quads");
        EditModeController::instance()->convertToQuads();
    });
    QAction* convertToQuadsAction = ui->objectsToolbar->addWidget(convertToQuadsButton);

    // Vertex paint: toggle on main click; arrow opens brush settings (color, radius, strength).
    auto makeVertexPaintBrushIcon = []() -> QIcon {
        // Do not rely on SVG icon plugins being present in packaged builds.
        // Paint a small green brush icon that matches the topology tool theme.
        constexpr int kSize = 18;
        QPixmap pm(kSize, kSize);
        pm.fill(Qt::transparent);

        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QColor accent(0x7A, 0xBD, 0x2A);      // matches topo selected state
        const QColor accentDark(0x58, 0x8F, 0x1E);
        const QColor metal(0xC8, 0xCF, 0xDB);
        const QColor metalDark(0x8B, 0x93, 0xA1);

        // Handle
        p.setPen(QPen(accentDark, 2.4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(13.8, 3.3), QPointF(7.1, 10.0));

        // Ferrule
        p.setPen(QPen(metalDark, 2.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(6.7, 10.4), QPointF(5.1, 12.0));

        // Bristles (filled)
        QPainterPath tip;
        tip.moveTo(3.4, 12.1);
        tip.lineTo(5.7, 13.3);
        tip.lineTo(4.1, 16.1);
        tip.lineTo(1.9, 14.9);
        tip.closeSubpath();
        p.fillPath(tip, metal);
        p.setPen(QPen(accent, 1.0));
        p.drawPath(tip);

        return QIcon(pm);
    };

    auto* vertexPaintButton = new QToolButton(ui->objectsToolbar);
    vertexPaintButton->setCheckable(true);
    vertexPaintButton->setIcon(makeVertexPaintBrushIcon());
    vertexPaintButton->setIconSize(QSize(18, 18));
    vertexPaintButton->setToolTip(tr("Vertex paint — paint on mesh (Select tool). Arrow: brush settings."));
    vertexPaintButton->setFont(topoFont);
    vertexPaintButton->setStyleSheet(topoBtnStyle);
    vertexPaintButton->setPopupMode(QToolButton::MenuButtonPopup);

    auto* vertexPaintMenu = new QMenu(vertexPaintButton);
    auto* paintSettings = new QWidget(vertexPaintMenu);
    auto* paintLay = new QVBoxLayout(paintSettings);
    paintLay->setContentsMargins(10, 8, 10, 8);
    paintLay->setSpacing(8);

    auto* emPaint = EditModeController::instance();

    auto* colorRow = new QHBoxLayout();
    colorRow->addWidget(new QLabel(tr("Color:"), paintSettings));
    auto* colorBtn = new QPushButton(paintSettings);
    colorBtn->setFixedSize(52, 24);
    auto syncPaintColorBtn = [colorBtn, emPaint]() {
        const QColor c = emPaint->vertexPaintColor();
        colorBtn->setStyleSheet(
            QStringLiteral("background-color: %1; border: 1px solid #888; border-radius: 3px;")
                .arg(c.name(QColor::HexRgb)));
    };
    syncPaintColorBtn();
    connect(colorBtn, &QPushButton::clicked, this, [this, emPaint, syncPaintColorBtn]() {
        SentryReporter::addBreadcrumb("ui.action", "Vertex paint color picker opened");
        QColor c = QColorDialog::getColor(emPaint->vertexPaintColor(), this, tr("Brush color"));
        if (c.isValid())
            emPaint->setVertexPaintColor(c);
        syncPaintColorBtn();
    });
    connect(emPaint, &EditModeController::vertexPaintChanged, this, syncPaintColorBtn);
    colorRow->addWidget(colorBtn);
    colorRow->addStretch();
    paintLay->addLayout(colorRow);

    static const char* kPaintSwatches[] = {
        "#ffffff", "#cccccc", "#888888", "#444444", "#000000",
        "#ff0000", "#ff8800", "#ffff00", "#88ff00", "#00ff00",
        "#00ff88", "#00ffff", "#0088ff", "#0000ff", "#8800ff",
        "#ff00ff", "#ff0088", "#8b4513", "#ffd700", "#90ee90",
        "#ff6347", "#00ced1", "#dda0dd", "#f0e68c"
    };
    auto* swatchGrid = new QGridLayout();
    swatchGrid->setSpacing(3);
    for (size_t i = 0; i < sizeof(kPaintSwatches) / sizeof(kPaintSwatches[0]); ++i) {
        const int r = static_cast<int>(i) / 8;
        const int col = static_cast<int>(i) % 8;
        auto* sw = new QPushButton(paintSettings);
        sw->setFixedSize(22, 22);
        const QString hex = QString::fromUtf8(kPaintSwatches[i]);
        sw->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid #666; border-radius: 2px;").arg(hex));
        connect(sw, &QPushButton::clicked, this, [emPaint, hex, syncPaintColorBtn]() {
            emPaint->setVertexPaintBrushColor(hex);
            syncPaintColorBtn();
        });
        swatchGrid->addWidget(sw, r, col);
    }
    paintLay->addLayout(swatchGrid);

    auto* radLabel = new QLabel(paintSettings);
    auto* radSlider = new QSlider(Qt::Horizontal, paintSettings);
    radSlider->setRange(2, 200);
    auto syncRad = [radLabel, radSlider, emPaint]() {
        QSignalBlocker b(radSlider);
        const int v = qBound(2, static_cast<int>(qRound(emPaint->vertexPaintRadius() * 100.0)), 200);
        radSlider->setValue(v);
        radLabel->setText(tr("Radius (local): %1").arg(emPaint->vertexPaintRadius(), 0, 'f', 2));
    };
    syncRad();
    connect(radSlider, &QSlider::valueChanged, this, [this, emPaint, radLabel](int v) {
        emPaint->setVertexPaintRadius(v / 100.0);
        radLabel->setText(tr("Radius (local): %1").arg(emPaint->vertexPaintRadius(), 0, 'f', 2));
    });
    connect(emPaint, &EditModeController::vertexPaintChanged, this, syncRad);
    paintLay->addWidget(radLabel);
    paintLay->addWidget(radSlider);

    auto* strLabel = new QLabel(paintSettings);
    auto* strSlider = new QSlider(Qt::Horizontal, paintSettings);
    strSlider->setRange(0, 100);
    auto syncStr = [strLabel, strSlider, emPaint]() {
        QSignalBlocker b(strSlider);
        strSlider->setValue(qBound(0, static_cast<int>(qRound(emPaint->vertexPaintStrength() * 100.0)), 100));
        strLabel->setText(tr("Strength: %1").arg(emPaint->vertexPaintStrength(), 0, 'f', 2));
    };
    syncStr();
    connect(strSlider, &QSlider::valueChanged, this, [this, emPaint, strLabel](int v) {
        emPaint->setVertexPaintStrength(v / 100.0);
        strLabel->setText(tr("Strength: %1").arg(emPaint->vertexPaintStrength(), 0, 'f', 2));
    });
    connect(emPaint, &EditModeController::vertexPaintChanged, this, syncStr);
    paintLay->addWidget(strLabel);
    paintLay->addWidget(strSlider);

    // Falloff slider (0..1)
    auto* falloffLabel = new QLabel(paintSettings);
    auto* falloffSlider = new QSlider(Qt::Horizontal, paintSettings);
    falloffSlider->setRange(0, 100);
    auto syncFalloff = [falloffLabel, falloffSlider, emPaint]() {
        QSignalBlocker b(falloffSlider);
        falloffSlider->setValue(qBound(0, static_cast<int>(qRound(emPaint->vertexPaintFalloff() * 100.0)), 100));
        falloffLabel->setText(tr("Falloff: %1").arg(emPaint->vertexPaintFalloff(), 0, 'f', 2));
    };
    syncFalloff();
    connect(falloffSlider, &QSlider::valueChanged, this, [this, emPaint, falloffLabel](int v) {
        emPaint->setVertexPaintFalloff(v / 100.0);
        falloffLabel->setText(tr("Falloff: %1").arg(emPaint->vertexPaintFalloff(), 0, 'f', 2));
    });
    connect(emPaint, &EditModeController::vertexPaintChanged, this, syncFalloff);
    paintLay->addWidget(falloffLabel);
    paintLay->addWidget(falloffSlider);

    auto* paintWa = new QWidgetAction(vertexPaintMenu);
    paintWa->setDefaultWidget(paintSettings);
    vertexPaintMenu->addAction(paintWa);
    vertexPaintButton->setMenu(vertexPaintMenu);

    connect(vertexPaintMenu, &QMenu::aboutToShow, this, [syncPaintColorBtn, syncRad, syncStr, syncFalloff]() {
        syncPaintColorBtn();
        syncRad();
        syncStr();
        syncFalloff();
    });

    connect(vertexPaintButton, &QToolButton::toggled, this, [this](bool on) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Toolbar: Vertex paint %1").arg(on ? QStringLiteral("on") : QStringLiteral("off")));
        EditModeController::instance()->setVertexPaintEnabled(on);
        if (on)
            setTransformState(TransformOperator::TS_SELECT);
    });
    connect(EditModeController::instance(), &EditModeController::vertexPaintChanged, this, [vertexPaintButton]() {
        QSignalBlocker b(vertexPaintButton);
        vertexPaintButton->setChecked(EditModeController::instance()->vertexPaintEnabled());
    });

    QAction* vertexPaintAction = ui->objectsToolbar->addWidget(vertexPaintButton);

    // Context-aware visibility + enabled:
    //  - Hidden entirely when NOT in edit mode.
    //  - In edit mode: stay visible but only enable when the current
    //    mode matches AND the relevant element type actually has a
    //    non-empty selection.
    auto refreshTopoButtons = [extrudeButton, bevelButton, knifeButton, mergeButton, deleteButton,
                               subdivideButton, fillButton, loopCutButton, convertToQuadsButton,
                               vertexPaintButton,
                               extrudeAction, bevelAction, knifeAction, mergeAction, deleteAction,
                               subdivideAction, fillAction, loopCutAction, convertToQuadsAction,
                               vertexPaintAction]() {
        auto* c = EditModeController::instance();
        const bool active = c->isEditModeActive();
        extrudeAction->setVisible(active);
        bevelAction->setVisible(active);
        knifeAction->setVisible(active);
        mergeAction->setVisible(active);
        deleteAction->setVisible(active);
        subdivideAction->setVisible(active);
        fillAction->setVisible(active);
        loopCutAction->setVisible(active);
        convertToQuadsAction->setVisible(active);
        vertexPaintAction->setVisible(active);
        if (!active) return;
        const int mode = c->selectionMode();  // 0 vertex, 1 edge, 2 face
        const bool hasFaces = c->selectedFaceCount() > 0;
        const bool hasEdges = c->selectedEdgeCount() > 0;
        const bool hasVerts = c->selectedVertexCount() > 0;
        extrudeButton->setEnabled(mode == 2 && hasFaces);
        bevelButton->setEnabled((mode == 1 && hasEdges)
                             || (mode == 0 && hasVerts));
        // Knife is always enabled in edit mode; the session has its own
        // point-based hit-tests and doesn't need a pre-existing selection.
        knifeButton->setEnabled(true);
        // Merge needs a multi-vertex selection in vertex mode — the four
        // operations (Center/First/Last/ByDistance) all require ≥2 verts.
        mergeButton->setEnabled(mode == 0 && c->selectedVertexCount() >= 2);
        // Delete works in any mode; enable when the matching selection is
        // non-empty.
        deleteButton->setEnabled((mode == 0 && hasVerts)
                              || (mode == 1 && hasEdges)
                              || (mode == 2 && hasFaces));
        // Subdivide: enabled whenever in edit mode. The Standard option
        // self-gates on a face/edge selection (no-op otherwise);
        // Catmull-Clark operates on the whole mesh and never needs a
        // selection.
        subdivideButton->setEnabled(true);
        // Fill: needs ≥3 verts (vertex mode) or ≥3 edges that form a
        // closed loop (edge mode — degree check happens at apply time).
        fillButton->setEnabled((mode == 0 && c->selectedVertexCount() >= 3)
                            || (mode == 1 && c->selectedEdgeCount() >= 3));
        // Loop cut: edge mode with at least one selected edge. The op
        // uses the first selected edge as the start; multi-edge loop
        // cuts aren't in scope for the MVP.
        loopCutButton->setEnabled(mode == 1 && hasEdges);
        // Convert to Quads: whole-mesh; disable only when EVERY submesh
        // is already n-gon canonical. Mixed meshes (some submeshes tri,
        // some quad) still qualify — the tri-only submeshes can still
        // be merged. (CodeRabbit follow-up on PR #347.)
        convertToQuadsButton->setEnabled(c->canConvertToQuads());
        vertexPaintButton->setEnabled(true);
    };
    refreshTopoButtons();
    connect(editCtrlForTopo, &EditModeController::editModeChanged,
            this, refreshTopoButtons);
    connect(editCtrlForTopo, &EditModeController::selectionModeChanged,
            this, refreshTopoButtons);
    connect(editCtrlForTopo, &EditModeController::editSelectionChanged,
            this, refreshTopoButtons);
    // Mesh-data changes (extrude/bevel/convertToQuads/undo) flip the
    // n-gon-vs-tri state — refresh so "Convert to Quads" disables once
    // the mesh has been promoted.
    connect(editCtrlForTopo, &EditModeController::meshDataChanged,
            this, refreshTopoButtons);

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

    // Sub-entity selection highlight (auto-connects to SelectionSet signals)
    SubEntityHighlight::getSingleton();

    // show mesh info overlay
    m_meshInfoOverlay = new MeshInfoOverlay(this);
    connect(ui->actionShow_Mesh_Info, &QAction::toggled, m_meshInfoOverlay, &MeshInfoOverlay::setVisible);
    // Sync menu checkmark when MCP or other code toggles the overlay directly
    connect(m_meshInfoOverlay, &MeshInfoOverlay::visibilityChanged, ui->actionShow_Mesh_Info, &QAction::setChecked);
    // Connect viewports created before the overlay existed
    for (EditorViewport* vp : mDockWidgetList)
        connect(vp->getOgreWidget(), &OgreWidget::focusOnWidget, m_meshInfoOverlay, &MeshInfoOverlay::setActiveWidget);

    // Asset Browser dock toggle via View menu
    connect(ui->actionAsset_Browser, &QAction::toggled, this, [this](bool checked) {
        SentryReporter::addBreadcrumb("ui.action",
            checked ? "Asset Browser shown" : "Asset Browser hidden");
        if (m_assetBrowserDock) {
            m_assetBrowserDock->setVisible(checked);
        }
    });
    // Sync menu checkmark when dock is closed via its title bar
    if (m_assetBrowserDock) {
        connect(m_assetBrowserDock, &QDockWidget::visibilityChanged,
                ui->actionAsset_Browser, &QAction::setChecked);
    }

    // Connect Browse button to a native file dialog (must be parented to MainWindow on macOS)
    connect(AssetBrowserController::instance(), &AssetBrowserController::browseRequested,
            this, [this]() {
        QTimer::singleShot(0, this, [this]() {
            QString dir = QFileDialog::getExistingDirectory(
                this, tr("Select Asset Directory"),
                AssetBrowserController::instance()->rootPath(),
                QFileDialog::DontUseNativeDialog | QFileDialog::ShowDirsOnly);
            if (!dir.isEmpty())
                AssetBrowserController::instance()->setRootPath(dir);
        });
    });

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
    aiMenu->setObjectName("menuAI");
    QAction* aiChatAction = aiMenu->addAction(QIcon(":/icones/ai.png"), tr("AI Chat..."));
    aiChatAction->setObjectName("actionAIChatDock");
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

    // Keyboard Shortcuts reference in Help menu
    QAction* shortcutsAction = ui->menuHelp->addAction(tr("Keyboard Shortcuts"));
    shortcutsAction->setObjectName("actionKeyboardShortcuts");
    shortcutsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Slash));
    connect(shortcutsAction, &QAction::triggered, this, [this]() {
        SentryReporter::addBreadcrumb("ui.action", "Help > Keyboard Shortcuts opened");

        auto* widget = new QQuickWidget(this);
        widget->setResizeMode(QQuickWidget::SizeRootObjectToView);
        widget->setSource(QUrl("qrc:/ShortcutReference/ShortcutReference.qml"));
        widget->setAttribute(Qt::WA_DeleteOnClose);
        widget->setMinimumSize(520, 560);
        widget->resize(520, 560);
        widget->setWindowFlags(Qt::Dialog);
        widget->setWindowTitle(tr("Keyboard Shortcuts"));
        widget->show();
    });

    // Preferences dialog (Edit > Preferences, Ctrl+,)
    connect(ui->actionPreferences, &QAction::triggered, this, [this]() {
        SentryReporter::addBreadcrumb("ui.action", "Edit > Preferences opened");

        auto* widget = new QQuickWidget(this);
        widget->setResizeMode(QQuickWidget::SizeRootObjectToView);
        widget->setSource(QUrl("qrc:/PreferencesDialog/PreferencesDialog.qml"));
        widget->setAttribute(Qt::WA_DeleteOnClose);
        widget->setMinimumSize(480, 520);
        widget->resize(480, 520);
        widget->setWindowFlags(Qt::Dialog);
        widget->setWindowTitle(tr("Preferences"));
        widget->show();
    });

    // Crash reporting toggle in Help menu
    ui->menuHelp->addSeparator();
    QAction* crashReportAction = ui->menuHelp->addAction(tr("Send Crash Reports"));
    crashReportAction->setObjectName("actionCrashReports");
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
    // Advance time for every entity that has enabled animation states.
    // Speed is global (scales dt for all states). The loop region applies only
    // to the entity+animation selected in the Animation Control panel.
    if(isPlaying)
    {
        const auto* animCtrl = AnimationControlController::instance();
        const std::string activeEntity = animCtrl->selectedEntityName().toStdString();
        const std::string activeAnim   = animCtrl->selectedAnimation().toStdString();
        const auto dt = static_cast<double>(evt.timeSinceLastFrame);
        const double scaledDt = dt * animCtrl->playbackSpeed();
        for(Ogre::SceneNode* node : Manager::getSingleton()->getSceneNodes())
        {
            if(!node) continue;
            for(int i = 0; i < static_cast<int>(node->numAttachedObjects()); ++i)
            {
                Ogre::MovableObject* obj = node->getAttachedObject(i);
                if(!obj || obj->getMovableType() != "Entity") continue;

                auto* ent = static_cast<Ogre::Entity*>(obj);
                const bool isActiveEntity = (!activeEntity.empty() && ent->getName() == activeEntity);
                Ogre::AnimationStateSet const* set = ent->getAllAnimationStates();
                if(!set) continue;
                for(const auto& [key, value] : set->getAnimationStates())
                {
                    if(!value->getEnabled()) continue;
                    if (isActiveEntity && key == activeAnim) {
                        // Selected animation: speed + loop region wrap.
                        const auto now  = static_cast<double>(value->getTimePosition());
                        const double next = animCtrl->advanceTime(now, dt);
                        value->setTimePosition(static_cast<float>(next));
                    } else {
                        // All other animations: speed only, no loop region.
                        value->addTime(static_cast<float>(scaledDt));
                    }
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
        // Auto-hide the welcome screen when a file is loaded
        if (m_welcomeController && m_welcomeController->isVisible())
            m_welcomeController->setVisible(false);

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

void MainWindow::duplicateSelected()
{
    SentryReporter::addBreadcrumb("ui.action", "Duplicate selected objects");

    SelectionSet* sel = SelectionSet::getSingleton();
    if (!sel || sel->getNodesCount() == 0) return;

    QList<Ogre::SceneNode*> sources = sel->getNodesSelectionList();
    QList<Ogre::SceneNode*> clones;

    for (Ogre::SceneNode* src : sources) {
        Ogre::SceneNode* clone = Manager::getSingleton()->duplicateSceneNode(src);
        if (clone) clones.append(clone);
    }

    if (!clones.isEmpty()) {
        UndoManager::getSingleton()->push(new DuplicateCommand(sources, clones));

        // Select the clones instead of the originals
        sel->clearList();
        for (Ogre::SceneNode* clone : clones)
            sel->append(clone);
    }
}

void MainWindow::groupSelected()
{
    SentryReporter::addBreadcrumb("ui.action", "Group selected nodes");

    SelectionSet* sel = SelectionSet::getSingleton();
    if (!sel || sel->getNodesCount() < 2) return;

    QList<Ogre::SceneNode*> nodes = sel->getNodesSelectionList();
    Ogre::SceneNode* groupNode = Manager::getSingleton()->groupNodes(nodes);
    if (groupNode)
        UndoManager::getSingleton()->push(new GroupCommand(nodes, nullptr));
}

void MainWindow::ungroupSelected()
{
    SentryReporter::addBreadcrumb("ui.action", "Ungroup selected node");

    SelectionSet* sel = SelectionSet::getSingleton();
    if (!sel || sel->getNodesCount() != 1) return;

    Ogre::SceneNode* node = sel->getSceneNode(0);
    if (!Manager::getSingleton()->isGroupNode(node)) return;

    UndoManager::getSingleton()->push(new UngroupCommand(node));
    Manager::getSingleton()->ungroupNode(node);
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    QtInputManager::getInstance().keyPressEvent(event);

    // Edit mode shortcuts take priority when edit mode is active
    auto* editCtrl = EditModeController::instance();

    // Esc cancels an active bevel session regardless of current tool.
    if (editCtrl->bevelSessionActive() && event->key() == Qt::Key_Escape) {
        SentryReporter::addBreadcrumb("ui.shortcut", "Esc — cancel bevel");
        editCtrl->cancelBevel();
        event->accept();
        return;
    }

    // Knife session: Esc cancels, Enter commits. Every other key is
    // swallowed while the session is open — otherwise selection-mode
    // shortcuts (1/2/3), Ctrl+E/B, Ctrl+A, and friends would silently
    // mutate state mid-cut and leave the user with a preview that no
    // longer matches the tool they're in. The user can't mean those
    // keys while the knife preview is on screen.
    if (editCtrl->knifeSessionActive()) {
        if (event->key() == Qt::Key_Escape) {
            SentryReporter::addBreadcrumb("ui.shortcut", "Esc — cancel knife");
            editCtrl->cancelKnife();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            SentryReporter::addBreadcrumb("ui.shortcut", "Enter — commit knife");
            editCtrl->commitKnife();
            event->accept();
            return;
        }
        // Swallow everything else so fall-through handlers don't fire.
        event->accept();
        return;
    }

    if (editCtrl->isEditModeActive()) {
        switch (event->key()) {
        case Qt::Key_1:
            SentryReporter::addBreadcrumb("ui.shortcut", "1 — Vertex selection mode");
            editCtrl->setSelectionMode(EditModeController::VertexMode);
            event->accept();
            return;
        case Qt::Key_2:
            SentryReporter::addBreadcrumb("ui.shortcut", "2 — Edge selection mode");
            editCtrl->setSelectionMode(EditModeController::EdgeMode);
            event->accept();
            return;
        case Qt::Key_3:
            SentryReporter::addBreadcrumb("ui.shortcut", "3 — Face selection mode");
            editCtrl->setSelectionMode(EditModeController::FaceMode);
            event->accept();
            return;
        case Qt::Key_A:
            if (event->modifiers() & Qt::ControlModifier) {
                SentryReporter::addBreadcrumb("ui.shortcut", "Ctrl+A — Select All (edit mode)");
                editCtrl->selectAll();
                event->accept();
                return;
            } else if (event->modifiers() & Qt::AltModifier) {
                SentryReporter::addBreadcrumb("ui.shortcut", "Alt+A — Deselect All (edit mode)");
                editCtrl->deselectAll();
                event->accept();
                return;
            }
            break;
        case Qt::Key_E:
            // Cmd+E (Ctrl+E on Linux/Windows): Extrude selection in face/edge mode
            if (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) {
                SentryReporter::addBreadcrumb("ui.shortcut", "Cmd+E — Extrude (edit mode)");
                editCtrl->extrudeSelection();
                event->accept();
                return;
            }
            // No modifier: fall through to rotate mode
            break;
        case Qt::Key_B:
            // Cmd+B (Ctrl+B on Linux/Windows): Bevel selection in edge mode
            if (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) {
                SentryReporter::addBreadcrumb("ui.shortcut", "Cmd+B — Bevel (edit mode)");
                editCtrl->bevelSelection();
                event->accept();
                return;
            }
            break;
        case Qt::Key_K:
            // K: enter knife mode. No modifier required — follows the
            // Blender convention for topology tools.
            if (!(event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier))) {
                SentryReporter::addBreadcrumb("ui.shortcut", "K — Knife (edit mode)");
                editCtrl->beginKnife();
                event->accept();
                return;
            }
            break;
        case Qt::Key_M:
            // M: Merge At Center on the current vertex selection. Phase-4
            // issue spec calls this out as the headline merge gesture; the
            // other targets (First / Last / By Distance) remain on the
            // toolbar dropdown. No modifier so it doesn't collide with
            // platform shortcuts (Cmd+M minimises on macOS — guarded).
            if (!(event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier))
                && editCtrl->selectionMode() == EditModeController::VertexMode
                && editCtrl->selectedVertexCount() >= 2) {
                SentryReporter::addBreadcrumb("ui.shortcut", "M — Merge At Center (edit mode)");
                editCtrl->mergeAtCenter();
                event->accept();
                return;
            }
            break;
        case Qt::Key_X: {
            // X: delete current edit-mode selection.
            // Ctrl/Cmd+X: dissolve current selection.
            // If nothing is selected we fall through so the outer Object-
            // mode "toggle World/Local space" handler runs — same gating
            // as the toolbar's refreshTopoButtons. (CodeRabbit Minor)
            const int mode = editCtrl->selectionMode();
            const bool hasSelection =
                   (mode == EditModeController::VertexMode && editCtrl->selectedVertexCount() > 0)
                || (mode == EditModeController::EdgeMode   && editCtrl->selectedEdgeCount()   > 0)
                || (mode == EditModeController::FaceMode   && editCtrl->selectedFaceCount()   > 0);
            if (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) {
                if (hasSelection) {
                    SentryReporter::addBreadcrumb("ui.shortcut", "Ctrl+X — Dissolve (edit mode)");
                    editCtrl->dissolveSelection();
                    event->accept();
                    return;
                }
                break;
            }
            if (!(event->modifiers() & (Qt::AltModifier | Qt::ShiftModifier))) {
                if (hasSelection) {
                    SentryReporter::addBreadcrumb("ui.shortcut", "X — Delete (edit mode)");
                    editCtrl->deleteSelection();
                    event->accept();
                    return;
                }
            }
            break;
        }
        default:
            break;
        }
    }

    switch(event->key()){
    case Qt::Key_Q:
        SentryReporter::addBreadcrumb("ui.shortcut", "Q — Select mode");
        setTransformState(TransformOperator::TS_SELECT);
       break;
    case Qt::Key_W:
        SentryReporter::addBreadcrumb("ui.shortcut", "W — Translate mode");
        setTransformState(TransformOperator::TS_TRANSLATE);
       break;
    case Qt::Key_E:
        SentryReporter::addBreadcrumb("ui.shortcut", "E — Rotate mode");
        setTransformState(TransformOperator::TS_ROTATE);
       break;
    case Qt::Key_R:
        // Ctrl+R in edit-mode + edge-selection: loop cut. Otherwise R
        // is Scale mode (Unity convention). The Ctrl modifier
        // disambiguates without disturbing the existing shortcut.
        if (event->modifiers() & Qt::ControlModifier) {
            auto* editCtrl = EditModeController::instance();
            if (editCtrl->isEditModeActive()
                && editCtrl->selectionMode() == EditModeController::EdgeMode
                && editCtrl->selectedEdgeCount() > 0) {
                // Always consume Ctrl+R when loop cut is the active
                // shortcut — even when the op short-circuits (e.g. tri
                // mesh). Falling through to Scale would silently switch
                // tools mid-loop-cut, which is what the user actually
                // pressed but isn't what they meant.
                editCtrl->loopCutSelection();
                SentryReporter::addBreadcrumb("ui.shortcut", "Ctrl+R — Loop Cut");
                event->accept();
                return;
            }
        }
        SentryReporter::addBreadcrumb("ui.shortcut", "R — Scale mode");
        setTransformState(TransformOperator::TS_SCALE);
       break;
    case Qt::Key_F:
    {
        // In edit mode, F fills selected vertices / edge loop (Blender
        // convention). Object mode + edit mode without a fillable
        // selection both fall through to "frame selection" (zoom-to-fit).
        auto* editCtrl = EditModeController::instance();
        if (editCtrl->isEditModeActive()) {
            const int mode = editCtrl->selectionMode();
            const bool fillable =
                   (mode == EditModeController::VertexMode
                    && editCtrl->selectedVertexCount() >= 3)
                || (mode == EditModeController::EdgeMode
                    && editCtrl->selectedEdgeCount() >= 3);
            if (fillable) {
                // Only swallow the keypress when the fill actually
                // produced geometry. Edge-mode selections that don't
                // form a closed boundary loop, or vertex selections
                // whose fan would duplicate an existing tri, return 0
                // — in those cases fall through so F still acts as
                // Frame-Selection. (Codex P2 / CodeRabbit Major)
                if (editCtrl->fillSelection() > 0) {
                    SentryReporter::addBreadcrumb("ui.shortcut", "F — Fill (edit mode)");
                    event->accept();
                    return;
                }
            }
        }
        SentryReporter::addBreadcrumb("ui.shortcut", "F — Frame selection");
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
        SentryReporter::addBreadcrumb("ui.shortcut", "X — Toggle transform space");
        TransformOperator::getSingleton()->toggleTransformSpace();
       break;
    case Qt::Key_P:
        SentryReporter::addBreadcrumb("ui.shortcut", "P — Cycle pivot mode");
        TransformOperator::getSingleton()->cyclePivotMode();
       break;
    case Qt::Key_Delete:
        if (!EditModeController::instance()->isEditModeActive()) {
            SentryReporter::addBreadcrumb("ui.shortcut", "Delete — Remove selected");
            TransformOperator::getSingleton()->removeSelected();
        }
       break;
    case Qt::Key_Tab:
        SentryReporter::addBreadcrumb("ui.shortcut", "Tab — Toggle Edit Mode");
        EditModeController::instance()->toggleEditMode();
        event->accept();
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

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (m_welcomeScreen && m_welcomeScreen->isVisible())
        repositionWelcomeScreen();
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

void MainWindow::loadFile(const QString& filePath)
{
    if (filePath.isEmpty()) return;
    addToRecentFiles(filePath);
    if (filePath.endsWith(".scene.glb") || filePath.endsWith(".scene.gltf"))
        MeshImporterExporter::sceneImporter(filePath);
    else
        mUriList.append(filePath);
}

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
                                                    tr("Scene Files (*.scene.glb *.scene.gltf);;glTF / VRM (*.gltf *.glb *.vrm);;All Files (*)"),
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
    // Any tool switch commits the current bevel session (user's explicit
    // choice — fires whether triggered by keyboard, menu, or toolbar).
    if (EditModeController::instance()->bevelSessionActive())
        EditModeController::instance()->commitBevel();

    ui->actionSelect_Object->setChecked(newState == TransformOperator::TS_SELECT);
    ui->actionTranslate_Object->setChecked(newState == TransformOperator::TS_TRANSLATE);
    ui->actionRotate_Object->setChecked(newState == TransformOperator::TS_ROTATE);
    ui->actionScale_Object->setChecked(newState == TransformOperator::TS_SCALE);

    if (newState != TransformOperator::TS_SELECT
        && EditModeController::instance()->vertexPaintEnabled()) {
        EditModeController::instance()->setVertexPaintEnabled(false);
    }

    TransformOperator::getSingleton()->onTransformStateChange(newState);
}

void MainWindow::updateEditModeIndicator()
{
    if (!m_editModeLabel) return;
    auto* ctrl = EditModeController::instance();
    if (ctrl->isEditModeActive()) {
        m_editModeLabel->setText("Edit Mode");
        m_editModeLabel->setStyleSheet(
            "QLabel { font-weight: bold; padding: 2px 8px; "
            "background-color: #3d6b3d; color: white; border-radius: 3px; }");
    } else {
        m_editModeLabel->setText("Object Mode");
        m_editModeLabel->setStyleSheet(
            "QLabel { font-weight: bold; padding: 2px 8px; }");
    }
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

void MainWindow::rebuildAllOgreViewports()
{
    for (EditorViewport* vp : mDockWidgetList) {
        if (OgreWidget* w = vp->getOgreWidget())
            w->rebuildRenderWindow();
    }
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
    int maxRecent = settings.value("General/recentFilesCount", 10).toInt();
    while (files.size() > maxRecent)
        files.removeLast();
    settings.setValue("RecentFiles/files", files);
    updateRecentFilesMenu();

    // Keep the welcome screen's recent files list in sync
    if (m_welcomeController)
        emit m_welcomeController->recentFilesChanged();
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
        if (m_welcomeController)
            emit m_welcomeController->recentFilesChanged();
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
        if (m_welcomeController)
            emit m_welcomeController->recentFilesChanged();
    }
}

// LCOV_EXCL_START — requires display
void MainWindow::showWelcomeScreen()
{
    if (!m_welcomeScreen) return;
    repositionWelcomeScreen();
    m_welcomeScreen->show();
    m_welcomeScreen->raise();
    m_welcomeScreen->setFocus();

    // Hide the ViewCube while welcome screen is showing (it has WindowStaysOnTopHint)
    if (m_viewCubeController)
        m_viewCubeController->setVisible(false);
}

void MainWindow::hideWelcomeScreen()
{
    if (m_welcomeScreen)
        m_welcomeScreen->hide();

    // Restore ViewCube visibility based on the menu toggle state
    if (m_viewCubeController && ui->actionShow_View_Cube->isChecked())
        m_viewCubeController->setVisible(true);
}

void MainWindow::repositionWelcomeScreen()
{
    if (!m_welcomeScreen) return;

    // Cover the entire main window — the QML overlay has its own
    // semi-transparent background that handles the visual layering.
    m_welcomeScreen->setGeometry(rect());
}
// LCOV_EXCL_STOP
