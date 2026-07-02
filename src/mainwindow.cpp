#include <QMessageBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>
#include <QInputDialog>
#include <QLineEdit>
#ifndef Q_OS_WIN
#include <unistd.h>
#endif
#include <QSettings>
#include <QSet>
#include <QApplication>
#include <QLibraryInfo>
#include <QEventLoop>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonArray>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QClipboard>
#include <QDesktopServices>
#include <QQuickWindow>
#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QJSEngine>
#include <QFileInfo>
#include <QEvent>
#include "SentryReporter.h"
#ifdef ENABLE_AUTO_UPDATER
#include "updater/UpdaterController.h"
#include "updater/UpdaterTelemetry.h"
#endif
#include <QDialog>
#include <QProgressDialog>
#include <QThread>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include "mainwindow.h"
#include "AppConsoleLog.h"
#include "AppSettingsKeys.h"
#include "CloudAccountMenuButton.h"
#include "CloudCredentialStore.h"
#include "CloudDeepLink.h"
#include "AppLaunchHandler.h"
#include "CloudProjectsController.h"
#include "CloudUploadDialog.h"
#include "CloudUploadPlanner.h"
#include "CloudUploadProgress.h"
#include "FeedbackDialog.h"
#include "FeedbackReportHelper.h"
#include "ProjectPackager.h"
#include "QtMeshCloudClient.h"
#include "QtMeshCloudSession.h"
#include "ui_mainwindow.h"
#include "OgreWidget.h"
#include "OgreRenderTargetUtil.h"
#include "QtInputManager.h"
#include "Manager.h"
#include <OgreCamera.h>

#include "material.h"
#include "about.h"
#include "PrimitivesWidget.h"
#include "MeshImporterExporter.h"
#include "EditorViewport.h"
#include "SpaceCamera.h"
#include "ViewportGrid.h"
#include "AnimationWidget.h"
#include "AnimationMerger.h"
#include "SelectionSet.h"
#include "AnimationBlender.h"
#include "AnimationControlController.h"
#include "CurveEditModel.h"
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
#ifdef ENABLE_ONNX
#include "AIAssistManager.h"
#endif
#ifdef ENABLE_PS1_RIP
#include "PS1/runtime/PS1RipSessionWindow.h"
#endif
#include "QMLMaterialHighlighter.h"
#include "ModelDownloader.h"
#include "UndoManager.h"
#include "commands/TransformCommands.h"
#include "PropertiesPanelController.h"
#include "MeshLodController.h"
#include "MeshDecimatorController.h"
#include "MeshValidator.h"
#include "AssetScanController.h"
#include "UvUnwrapController.h"
#include "UVEditorController.h"
#include "QuadRetopoController.h"
#include "SkinWeightsController.h"
#include "AutoRigController.h"
#include "MeshDepthRenderer.h"
#include "MaterialPresetLibrary.h"
#include "MaterialPreviewRenderer.h"
#include "AIChatManager.h"
#include "WelcomeScreenController.h"
#include "AssetBrowserController.h"
#include "EditModeController.h"
#include "TexturePaintController.h"
#include "PaintBufferImageProvider.h"
#include "VATBakerController.h"
#include "ThemeManager.h"
#include "IsometricSpritesController.h"
#include "ImageTo3D/MeshGenController.h"
#include "MorphAnimationManager.h"
#include "EditorModeController.h"
#include "QtMeshCloudClient.h"
#include <QDockWidget>
#include <QQuickWidget>
#include <QQmlContext>
#include <QToolButton>
#include <QStyle>
#include <QMenu>
#include <QWidgetAction>
#include <QSlider>
#include <QColorDialog>
#include <QSignalBlocker>
#include <QGridLayout>
#include <QToolBar>
#include <QAction>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QFontDatabase>
#include <QTimer>
#include <QDateTime>
#include <functional>
#include <atomic>

namespace {

constexpr int kBottomToolHeight = MainWindow::kDefaultDockedHeight;
constexpr int kBottomDockMaxHeight = MainWindow::kDefaultDockedMaxHeight;

constexpr char kLazyQmlUrlProperty[] = "_lazyQmlUrl";

void markLazyQml(QQuickWidget* widget, const QUrl& url)
{
    if (!widget)
        return;
    widget->setProperty(kLazyQmlUrlProperty, url);
}

void ensureLazyQml(QQuickWidget* widget)
{
    if (!widget || widget->status() != QQuickWidget::Null)
        return;
    const QVariant url = widget->property(kLazyQmlUrlProperty);
    if (url.isValid())
        widget->setSource(url.toUrl());
}

void ensureLazyDockQml(QDockWidget* dock)
{
    if (!dock)
        return;
    ensureLazyQml(qobject_cast<QQuickWidget*>(dock->widget()));
}

QString transformSpaceLabel(TransformOperator::TransformSpace space)
{
    return space == TransformOperator::SPACE_LOCAL
        ? QObject::tr("Local")
        : QObject::tr("World");
}

void registerEditorModeQmlSingletons()
{
    static bool registered = false;
    if (registered)
        return;

    qmlRegisterSingletonType<EditorModeController>("EditorMode", 1, 0, "EditorModeController",
        [](QQmlEngine* engine, QJSEngine*) -> QObject* {
            return EditorModeController::qmlInstance(engine, nullptr);
        });
    registered = true;
}

QString storedCloudDisplayName()
{
    QSettings settings;
    QString display = settings.value(AppSettingsKeys::cloudUserName()).toString().trimmed();
    if (display.isEmpty())
        display = settings.value(AppSettingsKeys::cloudUserSlug()).toString().trimmed();
    if (display.isEmpty())
        display = CloudCredentialStore::loadSession().email.trimmed();
    return display;
}

void waitWithEvents(int milliseconds, QProgressDialog* progress)
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    if (progress)
        QObject::connect(progress, &QProgressDialog::canceled, &loop, &QEventLoop::quit);
    timer.start(qMax(0, milliseconds));
    loop.exec();
}

QString primaryCloudAssetPath()
{
    QSettings settings;
    const QStringList recent = settings.value(QStringLiteral("RecentFiles/files")).toStringList();
    for (const QString& path : recent) {
        if (QFileInfo::exists(path))
            return path;
    }
    return {};
}

} // namespace

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

    // Sync palette menu checkboxes only — ThemeManager::applySavedThemeFromSettings()
    // in main() already applied the palette before any widgets existed. Re-calling
    // QApplication::setPalette() here (after initToolBar's QML docks) forces a
    // global repaint that can freeze the UI for tens of seconds.
    {
        QSettings settings;
        const QString appearanceTheme =
            settings.value(AppSettingsKeys::appearanceTheme()).toString().trimmed();
        const QString paletteTheme =
            settings.value(AppSettingsKeys::palette(), QStringLiteral("dark")).toString().trimmed();
        const QString paletteThemeLower = paletteTheme.toLower();
        mCurrentPalette =
            paletteThemeLower == QStringLiteral("custom")
                ? paletteTheme
                : (appearanceTheme.isEmpty() ? paletteTheme : appearanceTheme);
        ui->actionLight->blockSignals(true);
        ui->actionDark->blockSignals(true);
        ui->actionCustom->blockSignals(true);
        const QString themeLower = mCurrentPalette.trimmed().toLower();
        if (themeLower == QStringLiteral("light")) {
            ui->actionLight->setChecked(true);
        } else if (themeLower == QStringLiteral("custom")) {
            ui->actionCustom->setChecked(true);
        } else {
            ui->actionDark->setChecked(true);
        }
        ui->actionLight->blockSignals(false);
        ui->actionDark->blockSignals(false);
        ui->actionCustom->blockSignals(false);
    }

    initToolBar();

    FeedbackReportHelper::setOpenFeedbackHandler([this](const FeedbackPrefill& prefill) {
        showSendFeedbackDialog(prefill);
    });

    // Recent Files submenu in File menu
    m_recentFilesMenu = new QMenu(tr("Recent Files"), this); // NOSONAR — Qt parent manages lifetime
    m_recentFilesMenu->setObjectName("recentFilesMenu");
    ui->menuFile->insertMenu(ui->actionExport_Selected, m_recentFilesMenu);
    ui->menuFile->insertSeparator(ui->actionExport_Selected);

    m_cloudUploadMenuAction = new QAction(tr("Upload to QtMesh Cloud..."), this);
    m_cloudUploadMenuAction->setObjectName(QStringLiteral("actionUploadToQtMeshCloud"));
    connect(m_cloudUploadMenuAction, &QAction::triggered, this, [this]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("File menu: Upload to QtMesh Cloud"));
        uploadFilesToQtMeshCloud();
    });
    ui->menuFile->insertAction(ui->actionExport_Selected, m_cloudUploadMenuAction);

    m_cloudUploadProgress = new CloudUploadProgress(this);
    statusBar()->addPermanentWidget(m_cloudUploadProgress, 1);

    connect(CloudProjectsController::instance(), &CloudProjectsController::cloudProjectReady, this,
            [this](const QString& localMainFile) {
                m_cloudUploadProgress->finish(true, tr("Download complete"));
                closeCloudProjectsQmlDialog();
                QTimer::singleShot(0, this, [this, localMainFile]() {
                    importCloudDownloadedFile(localMainFile);
                    QTimer::singleShot(3000, m_cloudUploadProgress, &CloudUploadProgress::hideProgress);
                });
            });
    connect(CloudProjectsController::instance(), &CloudProjectsController::cloudOpenFailed, this,
            [this](const QString& error) {
                m_cloudUploadProgress->finish(false, tr("Download failed"));
                QMessageBox::warning(this, tr("QtMesh Cloud"), error);
                QTimer::singleShot(3000, m_cloudUploadProgress, &CloudUploadProgress::hideProgress);
            });
    connect(CloudProjectsController::instance(), &CloudProjectsController::cloudDownloadProgress, this,
            [this](int current, int total, const QString& fileName) {
                if (m_cloudUploadProgress->isHidden())
                    m_cloudUploadProgress->start(tr("Downloading from QtMesh Cloud…"), qMax(total, 1));
                m_cloudUploadProgress->updateProgress(current, qMax(total, 1), fileName);
            });
    connect(CloudProjectsController::instance(), &CloudProjectsController::signInRequired, this,
            [this]() {
                signInToQtMeshCloud();
                if (CloudCredentialStore::hasSession())
                    CloudProjectsController::instance()->refresh();
            });
    connect(CloudProjectsController::instance(), &CloudProjectsController::uploadRequested,
            this, &MainWindow::uploadFilesToQtMeshCloud);

    updateRecentFilesMenu();

    ui->menuOp_es->insertAction(ui->actionChange_Ambient_Light, ui->actionMaterial_Editor);
    ui->menuOp_es->insertSeparator(ui->actionChange_Ambient_Light);

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
                OgreRenderTargetUtil::restoreEditorRenderTarget();
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
    EditorModeController::instance();
    connect(EditModeController::instance(), &EditModeController::editModeChanged,
            this, &MainWindow::updateEditModeIndicator);
    connect(EditorModeController::instance(), &EditorModeController::modeChanged,
            this, &MainWindow::updateEditModeIndicator);
    connect(EditorModeController::instance(), &EditorModeController::statusTextChanged,
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

    AppConsoleLog::attachMainWindow(this);
}

/////////////////////////// TODO Clean up the code of MainWindow
/// /////////////////////// TODO improve the ui (toolbar, menubar,....) and add translation (obviously Portuguese but french, english, may be japaneese !)
MainWindow::~MainWindow()
{
    AppConsoleLog::detachMainWindow(this);

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
    EditorModeController::kill();
    Manager* manager = Manager::getSingletonPtr();
    if (manager) {
        // Paint controller holds an EditableMesh + ring overlay objects
        // owned by the SceneManager. Kill it before Manager teardown
        // so its destructor runs against a live Ogre.
        TexturePaintController::kill();
        EditModeController::kill();
        SubEntityHighlight::kill();
        AnimationBlender::kill();
        AnimationControlController::kill();
        CurveEditModel::kill();
        MeshLodController::kill();
        UvUnwrapController::kill();
        UVEditorController::kill();
        QuadRetopoController::kill();
        SkinWeightsController::kill();
        IsometricSpritesController::kill();
        MeshGenController::kill();
        MeshDepthRenderer::shutdown();
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
        ui->actionToggle_Transform_Space->setText(transformSpaceLabel(space));
    });
    ui->actionToggle_Transform_Space->setText(
        transformSpaceLabel(TransformOperator::getSingleton()->getTransformSpace()));

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
            // Force the animated entity to recompute skeleton derived
            // transforms so bone-visual TagPoints + skinning catch up
            // immediately. Without this, undo/redo of bone TRS edits
            // updates the data but the SkeletonDebug overlay stays at
            // its pre-undo pose until the next animation tick.
            auto* animCtrl = AnimationControlController::instance();
            // Drop cached track / keyframe pointers BEFORE refreshing
            // anything: AddKeyframeCommand::undo can destroy a track
            // entirely, and a stale m_selectedTrack would crash on the
            // next slider scrub.
            animCtrl->onUndoRedoCommandApplied();
            if (Ogre::Entity* ent = animCtrl->selectedEntity()) {
                if (Ogre::SkeletonInstance* skel = ent->getSkeleton()) {
                    // Force a full skeleton refresh so SkeletonDebug
                    // bone visuals + any TagPoint-attached entities
                    // pick up the post-undo pose immediately.
                    //   1. reset(true) — restore ALL bones (including
                    //      manual ones) to their initial state.
                    //   2. _updateAnimation — re-applies enabled
                    //      animation states + computes derived
                    //      transforms. Higher-level than calling
                    //      Animation::apply ourselves and handles
                    //      empty-track and missing-mask edge cases.
                    //   3. _updateTransforms — extra push to make
                    //      TagPoint-attached entities catch up.
                    skel->reset(true);
                    skel->_notifyManualBonesDirty();
                    ent->_updateAnimation();
                    // _updateAnimation correctly skips disabled
                    // animation states, leaving bones at bind pose
                    // when no animation is active — that's what the
                    // user expects (e.g. baking from a T-pose view
                    // should keep the T-pose). Don't second-guess
                    // it: an enabled-but-paused state still applies
                    // through _updateAnimation at its current time.
                    skel->_updateTransforms();
                }
            }
        });
    });

    // QML Properties Panel (replaces old Transform tab with modern collapsible inspector)
    {
        // NOTE: the Qt Quick *software* scene-graph backend is forced in main()
        // BEFORE QApplication (QSG_RHI_BACKEND / setGraphicsApi only take effect
        // before the scene graph initialises). Setting it here was too late and
        // left deployed-bundle dock QQuickWidgets rendering blank white.
        registerEditorModeQmlSingletons();

        m_propertiesPanel = new QQuickWidget();
        m_propertiesPanel->setResizeMode(QQuickWidget::SizeRootObjectToView);

        // Register QML singletons before setSource() so all imports resolve
        qmlRegisterSingletonType<PropertiesPanelController>("PropertiesPanel", 1, 0, "PropertiesPanelController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return PropertiesPanelController::qmlInstance(engine, nullptr);
            });
        qmlRegisterSingletonType<ThemeManager>("ThemeManager", 1, 0, "ThemeManager",
            [](QQmlEngine* engine, QJSEngine* scriptEngine) -> QObject* {
                return ThemeManager::qmlInstance(engine, scriptEngine);
            });
        qmlRegisterSingletonType<AnimationControlController>("AnimationControl", 1, 0, "AnimationControlController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return AnimationControlController::qmlInstance(engine, nullptr);
            });
        qmlRegisterSingletonType<AnimationBlender>("AnimationControl", 1, 0, "AnimationBlender",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return AnimationBlender::qmlInstance(engine, nullptr);
            });
        qmlRegisterSingletonType<CurveEditModel>("AnimationControl", 1, 0, "CurveEditModel",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return CurveEditModel::qmlInstance(engine, nullptr);
            });
        qmlRegisterSingletonType<MeshLodController>("PropertiesPanel", 1, 0, "MeshLodController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return MeshLodController::qmlInstance(engine, nullptr);
            });
        qmlRegisterSingletonType<MeshDecimatorController>(
            "PropertiesPanel", 1, 0, "MeshDecimatorController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return MeshDecimatorController::qmlInstance(engine, nullptr);
            });
        qmlRegisterSingletonType<UvUnwrapController>(
            "PropertiesPanel", 1, 0, "UvUnwrapController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return UvUnwrapController::qmlInstance(engine, nullptr);
            });
        qmlRegisterSingletonType<UVEditorController>(
            "PropertiesPanel", 1, 0, "UVEditorController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return UVEditorController::qmlInstance(engine, nullptr);
            });
        qmlRegisterSingletonType<QuadRetopoController>(
            "PropertiesPanel", 1, 0, "QuadRetopoController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return QuadRetopoController::qmlInstance(engine, nullptr);
            });
        qmlRegisterSingletonType<SkinWeightsController>(
            "PropertiesPanel", 1, 0, "SkinWeightsController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return SkinWeightsController::qmlInstance(engine, nullptr);
            });
        qmlRegisterSingletonType<AutoRigController>(
            "PropertiesPanel", 1, 0, "AutoRigController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return AutoRigController::qmlInstance(engine, nullptr);
            });
#ifdef ENABLE_AUTO_UPDATER
        qmlRegisterSingletonType<UpdaterController>(
            "Updater", 1, 0, "UpdaterController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return UpdaterController::qmlInstance(engine, nullptr);
            });
#endif
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
        qmlRegisterSingletonType<AssetScanController>("PropertiesPanel", 1, 0, "AssetScanController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return AssetScanController::qmlInstance(engine, nullptr);
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
        qmlRegisterSingletonType<TexturePaintController>("PropertiesPanel", 1, 0, "TexturePaintController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return TexturePaintController::qmlInstance(engine, nullptr);
            });
        qmlRegisterSingletonType<VATBakerController>("PropertiesPanel", 1, 0, "VATBakerController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return VATBakerController::qmlInstance(engine, nullptr);
            });
        qmlRegisterSingletonType<IsometricSpritesController>("PropertiesPanel", 1, 0, "IsometricSpritesController",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return IsometricSpritesController::qmlInstance(engine, nullptr);
            });
        connect(IsometricSpritesController::instance(), &IsometricSpritesController::outputPathPickRequested,
                this, [this](const QString &startPath) {
            QTimer::singleShot(0, this, [this, startPath]() {
                SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                              QStringLiteral("Isometric sprite save dialog requested"));
                const QString seed = IsometricSpritesController::normalizedSaveSeed(startPath);

                const QString chosen = QFileDialog::getSaveFileName(
                    this, tr("Save isometric sprite sheet"), seed,
                    tr("PNG image (*.png)"), nullptr,
                    QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons);
                SentryReporter::addBreadcrumb(
                    chosen.isEmpty() ? QStringLiteral("ui.action") : QStringLiteral("file.export"),
                    chosen.isEmpty() ? QStringLiteral("Isometric sprite save dialog cancelled")
                                     : QStringLiteral("Isometric sprite save dialog accepted: %1")
                                           .arg(chosen));
                emit IsometricSpritesController::instance()->outputPathPicked(chosen);
            });
        });
        // Registered under MaterialEditorQML (NOT PropertiesPanel): both
        // PropertiesPanel.qml and AISettingsDialog.qml already import
        // MaterialEditorQML, so a single registration resolves for both — and it
        // avoids registering the same C++ type under two module URIs (which
        // crashed MainWindow/MCPServer tests that reconstruct the window per test).
        qmlRegisterSingletonType<MeshGenController>("MaterialEditorQML", 1, 0, "MeshGenController",
            [](QQmlEngine* engine, QJSEngine* js) -> QObject* {
                return MeshGenController::create(engine, js);
            });
        qmlRegisterSingletonType<MorphAnimationManager>("PropertiesPanel", 1, 0, "MorphAnimationManager",
            [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                return MorphAnimationManager::qmlInstance(engine, nullptr);
            });

        // Same image provider the detached editor window uses — serves the
        // live paint buffer as a QImage view (no PNG encode, no base64).
        // Without this the Inspector thumbnail had to PNG-encode + reload
        // a brand-new data URI per refresh, which visibly blinked the
        // thumbnail during a paint stroke.
        m_propertiesPanel->engine()->addImageProvider(
            QStringLiteral("paintbuffer"), new PaintBufferImageProvider());

#ifdef ENABLE_AUTO_UPDATER
        connect(UpdaterController::instance(), &UpdaterController::showDialogRequested,
                this, [this](bool runCheck) { showUpdaterDialog(runCheck); });
        connect(UpdaterController::instance(), &UpdaterController::backgroundUpdateAvailable,
                this, [this](const QString& version) { showUpdateToast(version); });
#ifndef QTMESH_UNIT_TESTS
        QTimer::singleShot(5000, this, []() {
            UpdaterController::instance()->checkForUpdatesInBackground();
        });
#endif
#endif

        m_propertiesPanel->setSource(QUrl("qrc:/PropertiesPanel/PropertiesPanel.qml"));
        if (auto* root = m_propertiesPanel->rootObject()) {
            root->setProperty("bottomToolHost",
                              QVariant::fromValue(static_cast<QObject*>(this)));
        }
        createModeSurfaces();

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
        markLazyQml(chatWidget, QUrl("qrc:/AIChatPanel/AIChatPanel.qml"));
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
        assetBrowserWidget->setMinimumHeight(kBottomToolHeight);
        assetBrowserWidget->setMaximumHeight(kBottomToolHeight);
        assetBrowserWidget->setFocusPolicy(Qt::StrongFocus);
        markLazyQml(assetBrowserWidget, QUrl("qrc:/AssetBrowser/AssetBrowser.qml"));
        m_assetBrowserDock = new QDockWidget(tr("Asset Browser"), this);
        m_assetBrowserDock->setWidget(assetBrowserWidget);
        m_assetBrowserDock->setObjectName("AssetBrowserDock");
        configureBottomToolDock(m_assetBrowserDock);
        addDockWidget(Qt::BottomDockWidgetArea, m_assetBrowserDock);
        m_assetBrowserDock->hide();

        auto* abController = AssetBrowserController::instance();
        connect(abController, &AssetBrowserController::importMeshRequested, this,
                [this](const QStringList& paths) {
                    SentryReporter::addBreadcrumb("ui.action", "Asset Browser: import mesh");
                    importMeshs(paths);
                });
        connect(m_assetBrowserDock, &QDockWidget::visibilityChanged, this, [this](bool vis) {
            if (vis)
                ensureLazyDockQml(m_assetBrowserDock);
        });
    }

    // Dope Sheet dock — multi-bone keyframe view (Phase 5 slice C).
    // Qt takes parent ownership of QQuickWidget + QDockWidget — the same
    // pattern the rest of mainwindow.cpp uses. Sonar's S5025 wants smart
    // pointers, but raw `new` is idiomatic for Qt parent-owned widgets.
    {
        auto* dopeSheetWidget = new QQuickWidget(); // NOSONAR — Qt parent ownership
        dopeSheetWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
        dopeSheetWidget->setMinimumHeight(kBottomToolHeight);
        dopeSheetWidget->setMaximumHeight(kBottomToolHeight);
        dopeSheetWidget->setFocusPolicy(Qt::StrongFocus);
        markLazyQml(dopeSheetWidget, QUrl("qrc:/AnimationControl/AnimationDopeSheet.qml"));

        // QQuickWidget inside a QDockWidget on macOS can swallow wheel events
        // before they reach the QML scene's WheelHandler — Qt routes them to
        // the dock's title bar instead. Install a viewport-level filter that
        // calls back into the QML root's scrollByPixels() method.
        class DopeSheetWheelFilter : public QObject {
        public:
            explicit DopeSheetWheelFilter(QQuickWidget* host)
                : QObject(host), m_host(host) {}
        protected:
            bool eventFilter(QObject* watched, QEvent* event) override {
                if (event->type() != QEvent::Wheel) return QObject::eventFilter(watched, event);
                auto* we = static_cast<QWheelEvent*>(event);
                // Cmd/Ctrl+wheel falls through to QML's zoom WheelHandler.
                if (we->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) {
                    return QObject::eventFilter(watched, event);
                }
                if (!m_host || !m_host->rootObject()) return false;
                qreal dy = we->pixelDelta().y();
                if (dy == 0.0) dy = we->angleDelta().y() / 120.0 * 40.0;
                if (dy == 0.0) return false;
                QMetaObject::invokeMethod(m_host->rootObject(), "scrollByPixels",
                                          Q_ARG(QVariant, dy));
                event->accept();
                return true;
            }
        private:
            QQuickWidget* m_host;
        };
        auto* wheelFilter = new DopeSheetWheelFilter(dopeSheetWidget); // NOSONAR — Qt parent ownership
        dopeSheetWidget->installEventFilter(wheelFilter);
        m_dopeSheetDock = new QDockWidget(tr("Dope Sheet"), this); // NOSONAR — Qt parent ownership
        m_dopeSheetDock->setWidget(dopeSheetWidget);
        m_dopeSheetDock->setObjectName("DopeSheetDock");
        configureBottomToolDock(m_dopeSheetDock);
        addDockWidget(Qt::BottomDockWidgetArea, m_dopeSheetDock);
        m_dopeSheetDock->hide();
        connect(m_dopeSheetDock, &QDockWidget::visibilityChanged, this, [](bool vis) {
            SentryReporter::addBreadcrumb("ui.action",
                vis ? "Dope Sheet shown" : "Dope Sheet hidden");
        });

        // Reflect the active animation in the dock title — useful when the
        // dock is collapsed alongside other docks at the bottom.
        auto updateDopeSheetTitle = [this]() {
            if (!m_dopeSheetDock) return;
            const QString anim =
                AnimationControlController::instance()->selectedAnimation();
            m_dopeSheetDock->setWindowTitle(
                anim.isEmpty() ? tr("Dope Sheet")
                               : tr("Dope Sheet — %1").arg(anim));
        };
        connect(AnimationControlController::instance(),
                &AnimationControlController::selectionChanged,
                this, updateDopeSheetTitle);
        updateDopeSheetTitle();
    }

    // Curve Editor dock — Bezier-curve view (Phase 5 slice D3a, read-only).
    {
        auto* curveEditorWidget = new QQuickWidget(); // NOSONAR — Qt parent ownership
        curveEditorWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
        curveEditorWidget->setMinimumHeight(kBottomToolHeight);
        curveEditorWidget->setMaximumHeight(kBottomToolHeight);
        curveEditorWidget->setFocusPolicy(Qt::StrongFocus);
        markLazyQml(curveEditorWidget, QUrl("qrc:/AnimationControl/AnimationCurveEditor.qml"));
        m_curveEditorDock = new QDockWidget(tr("Curve Editor"), this); // NOSONAR
        m_curveEditorDock->setWidget(curveEditorWidget);
        m_curveEditorDock->setObjectName("CurveEditorDock");
        configureBottomToolDock(m_curveEditorDock);
        addDockWidget(Qt::BottomDockWidgetArea, m_curveEditorDock);
        // Tab on top of the dope sheet by default — the user toggles whichever
        // they want via the View menu. tabifyDockWidget runs after both docks
        // exist so we don't open two empty bottom strips.
        if (m_dopeSheetDock) tabifyDockWidget(m_dopeSheetDock, m_curveEditorDock);
        m_curveEditorDock->hide();
        connect(m_curveEditorDock, &QDockWidget::visibilityChanged, this, [](bool vis) {
            SentryReporter::addBreadcrumb("ui.action",
                vis ? "Curve Editor shown" : "Curve Editor hidden");
        });
    }

    // UV Editor lives in Material Mode → Mode Tools (UV Edit section) and
    // an optional detached window via UVEditorController::openEditorWindow().
    {
        auto* consoleContainer = new QWidget();
        auto* consoleLayout = new QVBoxLayout(consoleContainer);
        consoleLayout->setContentsMargins(4, 4, 4, 4);
        consoleLayout->setSpacing(4);
        m_consoleEdit = new QPlainTextEdit(consoleContainer);
        m_consoleEdit->setReadOnly(true);
        m_consoleEdit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        m_consoleEdit->setMaximumBlockCount(5000);
        m_consoleEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        auto* clearBtn = new QPushButton(tr("Clear"), consoleContainer);
        consoleLayout->addWidget(m_consoleEdit, 1);
        auto* btnRow = new QHBoxLayout();
        btnRow->addStretch();
        btnRow->addWidget(clearBtn);
        consoleLayout->addLayout(btnRow);
        connect(clearBtn, &QPushButton::clicked, m_consoleEdit, &QPlainTextEdit::clear);

        m_consoleDock = new QDockWidget(tr("Console"), this);
        m_consoleDock->setObjectName("ConsoleDock");
        m_consoleDock->setWidget(consoleContainer);
        configureBottomToolDock(m_consoleDock);
        addDockWidget(Qt::BottomDockWidgetArea, m_consoleDock);
        m_consoleDock->hide();
        connect(m_consoleDock, &QDockWidget::visibilityChanged, this, [](bool vis) {
            SentryReporter::addBreadcrumb("ui.action",
                vis ? "Console shown" : "Console hidden");
        });
    }

    tabifyBottomToolDocks();

    {
        QSettings viewPrefs;
        constexpr auto kContextKey = "View/showContextPanel";
        constexpr auto kConsoleKey = "View/showConsole";
        if (!viewPrefs.contains(kContextKey))
            viewPrefs.setValue(kContextKey, true);
        if (!viewPrefs.contains(kConsoleKey))
            viewPrefs.setValue(kConsoleKey, true);
        const bool wantContext = viewPrefs.value(kContextKey, true).toBool();
        const bool wantConsole = viewPrefs.value(kConsoleKey, true).toBool();

        if (wantConsole && m_consoleDock)
            showBottomToolDock(m_consoleDock);
        if (!wantConsole && m_consoleDock)
            m_consoleDock->hide();

        if (wantContext && m_bottomContextDock)
            showBottomToolDock(m_bottomContextDock);
        else if (m_bottomContextDock)
            m_bottomContextDock->hide();

        tabifyBottomToolDocks();
        if (wantContext && m_bottomContextDock)
            m_bottomContextDock->raise();
        else if (wantConsole && m_consoleDock)
            m_consoleDock->raise();
        // After tabification, Qt may still pick another tab on the first layout pass;
        // defer so Context stays selected when both docks are enabled.
        if (wantContext && m_bottomContextDock) {
            QTimer::singleShot(0, this, [this]() {
                if (m_bottomContextDock && !m_bottomContextDock->isHidden())
                    m_bottomContextDock->raise();
            });
        }
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

    // AI Chat — first on the contextual rail in every mode (before Add Primitive).
    auto aiChatButton = new QToolButton(ui->objectsToolbar);
    aiChatButton->setObjectName("aiChatToolbarButton");
    aiChatButton->setText("\u2728");  // ✨
    aiChatButton->setToolTip(tr("Open AI Chat"));
    QFont aiFont = aiChatButton->font();
    aiFont.setPixelSize(15);
    aiChatButton->setFont(aiFont);
    connect(aiChatButton, &QToolButton::clicked, this, [this]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Toolbar: Open AI Chat"));
        if (m_chatDock) {
            ensureLazyDockQml(m_chatDock);
            m_chatDock->show();
            m_chatDock->raise();
        }
    });
    QAction* aiChatToolbarAction = ui->objectsToolbar->addWidget(aiChatButton);
    aiChatToolbarAction->setObjectName("modeAnyAiChatAction");

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
    QAction* addPrimitiveAction = ui->objectsToolbar->addWidget(addPrimitiveButton);
    addPrimitiveAction->setObjectName("modeObjectPrimitiveAction");

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

    auto addRailButton = [this, topoBtnStyle](const QString& text,
                                              const QString& tooltip,
                                              const QString& actionObjectName,
                                              const std::function<void()>& triggered) {
        auto* button = new QToolButton(ui->objectsToolbar);
        button->setText(text);
        button->setToolTip(tooltip);
        QFont font = button->font();
        font.setPixelSize(text.size() > 1 ? 10 : 15);
        font.setBold(true);
        button->setFont(font);
        button->setStyleSheet(topoBtnStyle);
        connect(button, &QToolButton::clicked, this, triggered);

        QAction* action = ui->objectsToolbar->addWidget(button);
        action->setObjectName(actionObjectName);
        return action;
    };

    addRailButton(
        QStringLiteral("D"),
        tr("Show Dope Sheet"),
        QStringLiteral("modeAnimationDopeSheetAction"),
        [this]() {
            SentryReporter::addBreadcrumb("ui.action", "Rail: Dope Sheet");
            showBottomToolDock(m_dopeSheetDock);
        });

    addRailButton(
        QStringLiteral("C"),
        tr("Show Curve Editor"),
        QStringLiteral("modeAnimationCurveEditorAction"),
        [this]() {
            SentryReporter::addBreadcrumb("ui.action", "Rail: Curve Editor");
            showBottomToolDock(m_curveEditorDock);
        });

    addRailButton(
        QStringLiteral("UV"),
        tr("Open UV Editor"),
        QStringLiteral("modeUVEditorAction"),
        []() {
            SentryReporter::addBreadcrumb("ui.action", "Rail: UV Editor");
            EditorModeController::instance()->setCurrentMode(
                EditorModeController::MaterialMode);
            UVEditorController::instance()->openEditorWindow();
        });

    addRailButton(
        QStringLiteral("\u2713"),
        tr("Validate selected mesh"),
        QStringLiteral("modeValidationRunAction"),
        []() {
            SentryReporter::addBreadcrumb("ui.action", "Rail: Validate Mesh");
            MeshValidator::instance()->validate();
        });

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
    extrudeAction->setObjectName("modeEditExtrudeAction");

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
    bevelAction->setObjectName("modeEditBevelAction");

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
    knifeAction->setObjectName("modeEditKnifeAction");

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
    mergeAction->setObjectName("modeEditMergeAction");

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
    deleteAction->setObjectName("modeEditDeleteAction");

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
    subdivideAction->setObjectName("modeEditSubdivideAction");

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
    fillAction->setObjectName("modeEditFillAction");

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
    loopCutAction->setObjectName("modeEditLoopCutAction");

    // UV seam mark/clear (edge mode — issue #462).
    auto markSeamButton = new QToolButton(ui->objectsToolbar);
    markSeamButton->setText(QStringLiteral("\u2502"));  // │ seam line
    markSeamButton->setToolTip(tr("Mark UV Seam on selected edges"));
    markSeamButton->setFont(topoFont);
    markSeamButton->setStyleSheet(topoBtnStyle);
    connect(markSeamButton, &QToolButton::clicked, this, []() {
        SentryReporter::addBreadcrumb("ui.action", "Toolbar: Mark UV Seam");
        EditModeController::instance()->markSeamOnSelection();
    });
    QAction* markSeamAction = ui->objectsToolbar->addWidget(markSeamButton);
    markSeamAction->setObjectName("modeEditMarkSeamAction");

    auto clearSeamButton = new QToolButton(ui->objectsToolbar);
    clearSeamButton->setText(QStringLiteral("\u2500"));  // ─ clear seam
    clearSeamButton->setToolTip(tr("Clear UV Seam on selected edges"));
    clearSeamButton->setFont(topoFont);
    clearSeamButton->setStyleSheet(topoBtnStyle);
    connect(clearSeamButton, &QToolButton::clicked, this, []() {
        SentryReporter::addBreadcrumb("ui.action", "Toolbar: Clear UV Seam");
        EditModeController::instance()->clearSeamOnSelection();
    });
    QAction* clearSeamAction = ui->objectsToolbar->addWidget(clearSeamButton);
    clearSeamAction->setObjectName("modeEditClearSeamAction");

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
    convertToQuadsAction->setObjectName("modeEditConvertToQuadsAction");

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
    vertexPaintButton->setToolTip(tr("Paint brush — toggles vertex + texture paint together "
                                     "(Material Mode). Arrow: brush settings."));
    vertexPaintButton->setFont(topoFont);
    vertexPaintButton->setStyleSheet(topoBtnStyle);
    vertexPaintButton->setPopupMode(QToolButton::MenuButtonPopup);

    auto* vertexPaintMenu = new QMenu(vertexPaintButton);
    auto* paintSettings = new QWidget(vertexPaintMenu);
    auto* paintLay = new QVBoxLayout(paintSettings);
    paintLay->setContentsMargins(10, 8, 10, 8);
    paintLay->setSpacing(8);

    auto* emPaint = EditModeController::instance();

    // (Color selection deliberately removed from the brush popup —
    // the FG/BG swatch widget on the main toolbar is now the single
    // source of truth for the brush color. Duplicating it here would
    // just add a second place to keep in sync.)

    // Brush radius slider. Range: 0.001 → 2.0 in mesh-local units.
    // Mapped via /1000 so int slider ticks are 1mm resolution at the
    // low end (where users want pixel-precise dots) and still reach a
    // 2.0 unit radius for broad washes on tall meshes.
    auto* radLabel = new QLabel(paintSettings);
    auto* radSlider = new QSlider(Qt::Horizontal, paintSettings);
    radSlider->setRange(1, 2000);
    auto syncRad = [radLabel, radSlider, emPaint]() {
        QSignalBlocker b(radSlider);
        const int v = qBound(1, static_cast<int>(qRound(emPaint->vertexPaintRadius() * 1000.0)), 2000);
        radSlider->setValue(v);
        radLabel->setText(tr("Radius (local): %1").arg(emPaint->vertexPaintRadius(), 0, 'f', 3));
    };
    syncRad();
    connect(radSlider, &QSlider::valueChanged, this, [this, emPaint, radLabel](int v) {
        emPaint->setVertexPaintRadius(v / 1000.0);
        radLabel->setText(tr("Radius (local): %1").arg(emPaint->vertexPaintRadius(), 0, 'f', 3));
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

    // Brush shape selector: Round (circular falloff) vs Square
    // (axis-aligned constant strength, no falloff). Falloff slider
    // is ignored when Square is selected — kept enabled for
    // discoverability of "switch back to Round".
    auto* shapeRow = new QHBoxLayout();
    shapeRow->addWidget(new QLabel(tr("Shape:"), paintSettings));
    auto* shapeRound = new QPushButton(tr("Round"), paintSettings);
    auto* shapeSquare = new QPushButton(tr("Square"), paintSettings);
    shapeRound->setCheckable(true);
    shapeSquare->setCheckable(true);
    shapeRound->setAutoExclusive(true);
    shapeSquare->setAutoExclusive(true);
    shapeRound->setFixedHeight(22);
    shapeSquare->setFixedHeight(22);
    auto syncShape = [shapeRound, shapeSquare, emPaint]() {
        const bool square = emPaint->vertexPaintShape() == EditModeController::ShapeSquare;
        QSignalBlocker br(shapeRound);
        QSignalBlocker bs(shapeSquare);
        shapeRound->setChecked(!square);
        shapeSquare->setChecked(square);
    };
    syncShape();
    connect(shapeRound, &QPushButton::clicked, this, [emPaint]() {
        emPaint->setVertexPaintShape(static_cast<int>(EditModeController::ShapeRound));
    });
    connect(shapeSquare, &QPushButton::clicked, this, [emPaint]() {
        emPaint->setVertexPaintShape(static_cast<int>(EditModeController::ShapeSquare));
    });
    connect(emPaint, &EditModeController::vertexPaintChanged, this, syncShape);
    shapeRow->addWidget(shapeRound);
    shapeRow->addWidget(shapeSquare);
    shapeRow->addStretch();
    paintLay->addLayout(shapeRow);

    auto* paintWa = new QWidgetAction(vertexPaintMenu);
    paintWa->setDefaultWidget(paintSettings);
    vertexPaintMenu->addAction(paintWa);
    vertexPaintButton->setMenu(vertexPaintMenu);

    connect(vertexPaintMenu, &QMenu::aboutToShow, this, [syncRad, syncStr, syncFalloff]() {
        syncRad();
        syncStr();
        syncFalloff();
    });

    // The brush button is now a TOOL SELECTOR, not the paint-mode
    // master toggle. The Off/Vertex/Texture switch in the right
    // panel is the gate for enabling paint mode; the toolbar buttons
    // only pick which tool the user is using. Clicking the brush
    // button selects ToolPaint (or no-ops if it was already
    // selected — autoExclusive in spirit, the QToolButton group
    // logic is in syncBrushChecked below).
    connect(vertexPaintButton, &QToolButton::clicked, this, []() {
        SentryReporter::addBreadcrumb("ui.action", "Toolbar: tool = Paint");
        TexturePaintController::instance()->setBrushTool(
            static_cast<int>(TexturePaintController::ToolPaint));
    });
    // Tool button reflects "this tool is selected" rather than "paint
    // mode is on" — matches the wand button's pattern.
    auto syncPaintBtnChecked = [vertexPaintButton]() {
        const bool selected =
            TexturePaintController::instance()->brushTool()
                == static_cast<int>(TexturePaintController::ToolPaint);
        QSignalBlocker b(vertexPaintButton);
        vertexPaintButton->setChecked(selected);
    };
    syncPaintBtnChecked();
    connect(TexturePaintController::instance(),
            &TexturePaintController::brushToolChanged, this, syncPaintBtnChecked);

    QAction* vertexPaintAction = ui->objectsToolbar->addWidget(vertexPaintButton);
    vertexPaintAction->setObjectName("modeMaterialPaintBrushAction");

    // Wand (smart-select) toolbar button. Lives directly under the
    // paint brush so the user can swap between "paint" and "magic-
    // wand select" without leaving the toolbar. Click toggles between
    // the Paint tool and the Wand tool — paint stays the default
    // when the button is unchecked.
    //
    // Icon is drawn in two QIcon modes: green (Normal) and dimmed
    // grey (Disabled) — matching the green / grey rhythm of the
    // surrounding topology buttons.
    auto paintWand = [](QPixmap& pm, const QColor& color) {
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(3.0, 15.0), QPointF(13.5, 4.5));
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(14.4, 3.6), 2.1, 2.1);  // tip ball
        p.drawEllipse(QPointF(3.0, 15.0), 1.2, 1.2);  // handle nub
    };
    auto makeWandIcon = [paintWand]() -> QIcon {
        constexpr int kSize = 18;
        QPixmap onPm(kSize, kSize);
        paintWand(onPm, QColor(0x7B, 0xBD, 0x2A));  // green, matches topology buttons
        QPixmap offPm(kSize, kSize);
        paintWand(offPm, QColor(0xB8, 0xB8, 0xB8)); // grey for disabled
        QIcon icon;
        icon.addPixmap(onPm,  QIcon::Normal,   QIcon::Off);
        icon.addPixmap(onPm,  QIcon::Active,   QIcon::Off);
        icon.addPixmap(onPm,  QIcon::Selected, QIcon::Off);
        icon.addPixmap(onPm,  QIcon::Normal,   QIcon::On);
        icon.addPixmap(offPm, QIcon::Disabled, QIcon::Off);
        icon.addPixmap(offPm, QIcon::Disabled, QIcon::On);
        return icon;
    };

    auto* wandButton = new QToolButton(ui->objectsToolbar);
    wandButton->setCheckable(true);
    wandButton->setIcon(makeWandIcon());
    wandButton->setIconSize(QSize(18, 18));
    wandButton->setToolTip(tr("Smart-select (Wand)\n"
                              "Click a region to select pixels of similar color.\n"
                              "Drag horizontally while clicking to widen / narrow.\n"
                              "Click outside the mesh to clear the selection.\n"
                              "Enabled only in Texture paint mode."));
    wandButton->setFont(topoFont);
    wandButton->setStyleSheet(topoBtnStyle);

    auto syncWandChecked = [wandButton]() {
        const bool wandActive =
            TexturePaintController::instance()->brushTool()
                == static_cast<int>(TexturePaintController::ToolSmartSelect);
        QSignalBlocker b(wandButton);
        wandButton->setChecked(wandActive);
    };
    // Wand only does something when texture paint is on AND the target
    // is Texture (it operates on the paint buffer, not vertex colors).
    // The disabled icon comes from QIcon's Disabled mode pixmap added
    // in makeWandIcon, so the grey state matches the other toolbar
    // buttons' disabled treatment.
    auto syncWandEnabled = [wandButton]() {
        auto* tpc = TexturePaintController::instance();
        const bool canWand =
            tpc->texturePaintEnabled()
            && tpc->paintTarget() == static_cast<int>(TexturePaintController::TargetTexture);
        wandButton->setEnabled(canWand);
        if (!canWand && wandButton->isChecked()) {
            // Auto-uncheck so the next paint-mode re-entry starts clean.
            QSignalBlocker b(wandButton);
            wandButton->setChecked(false);
            tpc->setBrushTool(static_cast<int>(TexturePaintController::ToolPaint));
        }
    };
    syncWandChecked();
    syncWandEnabled();

    connect(wandButton, &QToolButton::toggled, this, [](bool on) {
        auto* tpc = TexturePaintController::instance();
        const int newTool = on
            ? static_cast<int>(TexturePaintController::ToolSmartSelect)
            : static_cast<int>(TexturePaintController::ToolPaint);
        tpc->setBrushTool(newTool);
        // Make sure paint mode is on while the user is reaching for
        // the wand — otherwise the toolbar toggle does nothing visible.
        if (on)
            tpc->setTexturePaintEnabled(true);
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Toolbar: Wand %1").arg(on ? "on" : "off"));
    });
    connect(TexturePaintController::instance(),
            &TexturePaintController::brushToolChanged, this, syncWandChecked);
    connect(TexturePaintController::instance(),
            &TexturePaintController::texturePaintChanged, this, syncWandEnabled);
    connect(TexturePaintController::instance(),
            &TexturePaintController::paintTargetChanged, this, syncWandEnabled);

    QAction* wandAction = ui->objectsToolbar->addWidget(wandButton);
    wandAction->setObjectName("modeMaterialWandAction");

    // Per-tool toolbar buttons (Bucket / Eraser / Eyedropper / Smudge)
    // below the wand. Same green-active / grey-disabled treatment as
    // the wand. Each is a TexturePaintController tool selector — it
    // doesn't enable paint mode; the Off/Vertex/Texture switch in
    // the right panel is the master gate.
    struct ToolButtonEntry {
        int tool;
        const char* label;
        const char* objectName;
        std::function<void(QPainter&, const QColor&)> paint;
    };

    // Drawing helpers. Same green/grey palette as the wand, drawn at
    // 18x18 — keep the icons high-contrast and silhouette-distinct so
    // users can pick them out in a vertical column without reading
    // tooltips.
    const std::vector<ToolButtonEntry> tools = {
        {
            static_cast<int>(TexturePaintController::ToolFill),
            "Bucket", "modeMaterialBucketAction",
            [](QPainter& p, const QColor& c) {
                // Bucket: trapezoid body + handle, with a paint drip.
                p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                QPainterPath bucket;
                bucket.moveTo(3.5, 6.5);
                bucket.lineTo(14.5, 6.5);
                bucket.lineTo(13.5, 14.5);
                bucket.lineTo(4.5, 14.5);
                bucket.closeSubpath();
                p.drawPath(bucket);
                // Rim
                p.drawLine(QPointF(2.8, 6.5), QPointF(15.2, 6.5));
                // Handle arc
                p.drawArc(QRectF(5.5, 2.5, 7, 5), 30 * 16, 120 * 16);
                // Drip
                p.setBrush(c);
                p.setPen(Qt::NoPen);
                p.drawEllipse(QPointF(11.5, 10.5), 1.5, 2.0);
            }
        },
        {
            static_cast<int>(TexturePaintController::ToolErase),
            "Eraser", "modeMaterialEraserAction",
            [](QPainter& p, const QColor& c) {
                // Eraser block: rounded rect with a diagonal split
                // between the rubber and the metal ferrule.
                p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                p.save();
                p.translate(9, 9);
                p.rotate(-30);
                p.drawRoundedRect(QRectF(-6, -3, 12, 6), 1.5, 1.5);
                // Inner ferrule line
                p.drawLine(QPointF(0, -3), QPointF(0, 3));
                p.restore();
            }
        },
        {
            static_cast<int>(TexturePaintController::ToolColorPicker),
            "Pick", "modeMaterialPickAction",
            [](QPainter& p, const QColor& c) {
                // Eyedropper: long diagonal pipette + bulb.
                p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(QPointF(3.0, 15.0), QPointF(10.0, 8.0));
                p.setBrush(c);
                p.setPen(Qt::NoPen);
                // Bulb (top end)
                p.save();
                p.translate(13, 5);
                p.rotate(45);
                p.drawRoundedRect(QRectF(-3, -2, 6, 4), 1, 1);
                p.restore();
                // Drip tip
                p.drawEllipse(QPointF(3.0, 15.0), 1.1, 1.1);
            }
        },
        {
            static_cast<int>(TexturePaintController::ToolSmudge),
            "Smudge", "modeMaterialSmudgeAction",
            [](QPainter& p, const QColor& c) {
                // Smudge: finger-tip outline + wavy smear trail.
                p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                // Finger pad as a rotated tear-drop
                QPainterPath finger;
                finger.moveTo(12, 4);
                finger.cubicTo(14, 6, 14, 10, 11, 12);
                finger.cubicTo(8, 13, 6, 10, 8, 8);
                finger.cubicTo(9, 6, 11, 4, 12, 4);
                p.drawPath(finger);
                // Smear trail
                QPainterPath smear;
                smear.moveTo(3, 14);
                smear.cubicTo(5, 12, 7, 16, 9, 14);
                p.drawPath(smear);
            }
        }
    };

    std::vector<QToolButton*> toolButtons;
    std::vector<QAction*> toolActions;
    toolButtons.reserve(tools.size());
    toolActions.reserve(tools.size());

    for (const auto& tdef : tools) {
        auto makeIcon = [paint = tdef.paint](const QColor& color) -> QPixmap {
            QPixmap pm(18, 18);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing, true);
            paint(p, color);
            return pm;
        };
        const QColor green(0x7B, 0xBD, 0x2A);
        const QColor disabled(0xB8, 0xB8, 0xB8);
        QIcon icon;
        const QPixmap onPm = makeIcon(green);
        const QPixmap offPm = makeIcon(disabled);
        icon.addPixmap(onPm,  QIcon::Normal,   QIcon::Off);
        icon.addPixmap(onPm,  QIcon::Active,   QIcon::Off);
        icon.addPixmap(onPm,  QIcon::Selected, QIcon::Off);
        icon.addPixmap(onPm,  QIcon::Normal,   QIcon::On);
        icon.addPixmap(offPm, QIcon::Disabled, QIcon::Off);
        icon.addPixmap(offPm, QIcon::Disabled, QIcon::On);

        auto* btn = new QToolButton(ui->objectsToolbar);
        btn->setCheckable(true);
        btn->setIcon(icon);
        btn->setIconSize(QSize(18, 18));
        btn->setToolTip(tr("%1 — paint tool").arg(tr(tdef.label)));
        btn->setFont(topoFont);
        btn->setStyleSheet(topoBtnStyle);

        const int targetTool = tdef.tool;
        connect(btn, &QToolButton::clicked, this, [targetTool, label = tdef.label]() {
            SentryReporter::addBreadcrumb("ui.action",
                QStringLiteral("Toolbar: tool = %1").arg(label));
            TexturePaintController::instance()->setBrushTool(targetTool);
        });

        QAction* act = ui->objectsToolbar->addWidget(btn);
        act->setObjectName(tdef.objectName);
        toolButtons.push_back(btn);
        toolActions.push_back(act);
    }

    // Sync the per-tool button check / enabled state. Tools other than
    // Paint and Wand only make sense when paint mode is on AND target
    // is Texture (vertex paint doesn't use flood-fill, picker, etc).
    //
    // Capture toolButtons AND the tool int IDs *by value* — the `tools`
    // descriptor vector is a local that disappears at the end of the
    // ctor, so binding it by reference would crash the next time the
    // user changed paint target / enable (the lambda would deref a
    // freed std::vector). Bug found mid-iteration: the Texture-button
    // click hit exactly this path.
    std::vector<int> toolIds;
    toolIds.reserve(tools.size());
    for (const auto& t : tools) toolIds.push_back(t.tool);

    auto syncToolButtons = [toolButtons, toolIds]() {
        auto* tpc = TexturePaintController::instance();
        const int cur = tpc->brushTool();
        const bool canTextureTool =
            tpc->texturePaintEnabled()
            && tpc->paintTarget() == static_cast<int>(TexturePaintController::TargetTexture);
        for (size_t i = 0; i < toolButtons.size() && i < toolIds.size(); ++i) {
            QSignalBlocker b(toolButtons[i]);
            toolButtons[i]->setChecked(cur == toolIds[i]);
            toolButtons[i]->setEnabled(canTextureTool);
        }
    };
    syncToolButtons();
    connect(TexturePaintController::instance(),
            &TexturePaintController::brushToolChanged, this, syncToolButtons);
    connect(TexturePaintController::instance(),
            &TexturePaintController::texturePaintChanged, this, syncToolButtons);
    connect(TexturePaintController::instance(),
            &TexturePaintController::paintTargetChanged, this, syncToolButtons);

    // FG/BG color swatch widget — Photoshop / GIMP style. Two overlapping
    // rectangles showing the foreground and background colors. The user
    // can click either to open a color picker, click the small swap
    // arrow to flip them, or use the tiny "reset" indicator to put back
    // FG=black, BG=white. Toolbar-resident so it's always one click away,
    // no popup needed.
    auto* paintColors = new QWidget(ui->objectsToolbar);
    paintColors->setFixedSize(34, 28);
    paintColors->setToolTip(tr("Foreground / Background colors\n"
                               "Click either swatch to change. The 'Erase' brush\n"
                               "paints with the BG color."));
    auto repaintSwatch = [paintColors]() { paintColors->update(); };
    paintColors->installEventFilter(this);
    // Custom paint via overridden paintEvent on a private subclass would
    // be cleaner, but a single inline filter avoids growing the class.
    // We instead set a stylesheet-free child for each swatch.
    {
        auto* bg = new QPushButton(paintColors);
        bg->setGeometry(11, 7, 18, 17);
        bg->setObjectName("paintBgSwatch");
        bg->setFlat(true);
        bg->setFocusPolicy(Qt::NoFocus);
        bg->setCursor(Qt::PointingHandCursor);
        bg->setToolTip(tr("Background color"));
        auto* fg = new QPushButton(paintColors);
        fg->setGeometry(3, 0, 18, 17);
        fg->setObjectName("paintFgSwatch");
        fg->setFlat(true);
        fg->setFocusPolicy(Qt::NoFocus);
        fg->setCursor(Qt::PointingHandCursor);
        fg->setToolTip(tr("Foreground color"));
        // Tiny FG/BG swap arrow in the corner. A unicode glyph keeps us
        // off icon-plugin dependencies.
        auto* swap = new QPushButton(paintColors);
        swap->setGeometry(20, 0, 14, 12);
        swap->setObjectName("paintSwap");
        swap->setFlat(true);
        swap->setFocusPolicy(Qt::NoFocus);
        swap->setText(QStringLiteral("⇄"));
        swap->setToolTip(tr("Swap foreground/background colors"));
        swap->setStyleSheet(QStringLiteral(
            "QPushButton { color: #aaa; background: transparent; border: none; font-size: 9px; padding: 0; }"
            "QPushButton:hover { color: #fff; }"));
        // Default-reset glyph: tiny black-over-white squares in the
        // opposite corner. Click puts FG=black, BG=white.
        auto* reset = new QPushButton(paintColors);
        reset->setGeometry(0, 18, 12, 10);
        reset->setObjectName("paintReset");
        reset->setFlat(true);
        reset->setFocusPolicy(Qt::NoFocus);
        reset->setText(QStringLiteral("◰"));
        reset->setToolTip(tr("Reset to default foreground/background"));
        reset->setStyleSheet(QStringLiteral(
            "QPushButton { color: #aaa; background: transparent; border: none; font-size: 10px; padding: 0; }"
            "QPushButton:hover { color: #fff; }"));

        auto syncSwatches = [fg, bg]() {
            auto* em = EditModeController::instance();
            const QColor fgC = em->vertexPaintColor();
            const QColor bgC = em->vertexPaintBackgroundColor();
            fg->setStyleSheet(QStringLiteral(
                "QPushButton { background-color: %1; border: 1px solid #444; border-radius: 2px; }")
                .arg(fgC.name(QColor::HexRgb)));
            bg->setStyleSheet(QStringLiteral(
                "QPushButton { background-color: %1; border: 1px solid #444; border-radius: 2px; }")
                .arg(bgC.name(QColor::HexRgb)));
        };
        syncSwatches();
        connect(EditModeController::instance(), &EditModeController::vertexPaintChanged,
                this, syncSwatches);

        connect(fg, &QPushButton::clicked, this, [this, syncSwatches]() {
            SentryReporter::addBreadcrumb("ui.action", "Toolbar: FG color picker opened");
            auto* em = EditModeController::instance();
            QColor c = QColorDialog::getColor(em->vertexPaintColor(), this,
                tr("Foreground color"),
                QColorDialog::ShowAlphaChannel);
            if (c.isValid())
                em->setVertexPaintColor(c);
            syncSwatches();
        });
        connect(bg, &QPushButton::clicked, this, [this, syncSwatches]() {
            SentryReporter::addBreadcrumb("ui.action", "Toolbar: BG color picker opened");
            auto* em = EditModeController::instance();
            QColor c = QColorDialog::getColor(em->vertexPaintBackgroundColor(), this,
                tr("Background color"),
                QColorDialog::ShowAlphaChannel);
            if (c.isValid())
                em->setVertexPaintBackgroundColor(c);
            syncSwatches();
        });
        connect(swap, &QPushButton::clicked, this, [syncSwatches]() {
            SentryReporter::addBreadcrumb("ui.action", "Toolbar: swap FG/BG colors");
            EditModeController::instance()->swapPaintColors();
            syncSwatches();
        });
        connect(reset, &QPushButton::clicked, this, [syncSwatches]() {
            SentryReporter::addBreadcrumb("ui.action", "Toolbar: reset FG/BG colors");
            EditModeController::instance()->resetPaintColors();
            syncSwatches();
        });
    }
    QAction* paintColorsAction = ui->objectsToolbar->addWidget(paintColors);
    paintColorsAction->setObjectName("modeMaterialPaintColorsAction");

    // The paint brush is contextual:
    //  - Material Mode → texture paint (paint into the BaseColor)
    //  - Edit Mode    → vertex paint (paint vertex colors)
    //  - Other modes  → hidden
    // Switching modes turns the previous mode's brush off so we don't
    // leave a stale checked state.
    auto refreshPaintBrushVisibility = [vertexPaintButton, vertexPaintAction,
                                        paintColorsAction, wandButton, wandAction,
                                        toolActions]() {
        const auto mode = EditorModeController::instance()->currentMode();
        const bool material = mode == EditorModeController::MaterialMode;
        // Paint brush lives in Material Mode only. The user chooses
        // between Vertex and Texture painting via the panel's target
        // picker; both run through TexturePaintController.
        vertexPaintAction->setVisible(material);
        vertexPaintButton->setEnabled(material);
        // FG/BG swatch stays available in Edit Mode too — vertex paint
        // runs from Edit Mode and the brush popup no longer holds a
        // color picker, so without this the user has no toolbar
        // access to the foreground color while painting vertex colors.
        const bool edit = mode == EditorModeController::EditMode;
        paintColorsAction->setVisible(material || edit);
        wandAction->setVisible(material);
        for (auto* a : toolActions)
            if (a) a->setVisible(material);
        // wandButton enabled state is driven by syncWandEnabled —
        // paint-on + target=Texture. Hide here when out of Material
        // Mode but don't overrule enabled.
        // Disable both brushes AND reset the button's checked state on
        // mode change. Without resetting the checked flag, the user's
        // next click toggles "on→off" (since we silently set the
        // controllers off but left the button visually checked).
        EditModeController::instance()->setVertexPaintEnabled(false);
        TexturePaintController::instance()->setTexturePaintEnabled(false);
        SentryReporter::addBreadcrumb(
            "ui.action",
            QStringLiteral("Mode switch: paint state reset"));
        // Reset the tool to Paint on every mode entry so the wand
        // toolbar button starts unchecked too.
        TexturePaintController::instance()->setBrushTool(
            static_cast<int>(TexturePaintController::ToolPaint));
        QSignalBlocker bw(wandButton);
        wandButton->setChecked(false);
    };
    refreshPaintBrushVisibility();
    connect(EditorModeController::instance(), &EditorModeController::modeChanged,
            this, refreshPaintBrushVisibility);

    // Context-aware visibility + enabled:
    //  - Hidden entirely when NOT in edit mode.
    //  - In edit mode: stay visible but only enable when the current
    //    mode matches AND the relevant element type actually has a
    //    non-empty selection.
    auto refreshTopoButtons = [extrudeButton, bevelButton, knifeButton, mergeButton, deleteButton,
                               subdivideButton, fillButton, loopCutButton, markSeamButton, clearSeamButton,
                               convertToQuadsButton,
                               extrudeAction, bevelAction, knifeAction, mergeAction, deleteAction,
                               subdivideAction, fillAction, loopCutAction, markSeamAction, clearSeamAction,
                               convertToQuadsAction]() {
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
        markSeamAction->setVisible(active);
        clearSeamAction->setVisible(active);
        convertToQuadsAction->setVisible(active);
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
        markSeamButton->setEnabled(mode == 1 && hasEdges);
        clearSeamButton->setEnabled(mode == 1 && hasEdges);
        // Convert to Quads: whole-mesh; disable only when EVERY submesh
        // is already n-gon canonical. Mixed meshes (some submeshes tri,
        // some quad) still qualify — the tri-only submeshes can still
        // be merged. (CodeRabbit follow-up on PR #347.)
        convertToQuadsButton->setEnabled(c->canConvertToQuads());
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
    connect(EditorModeController::instance(), &EditorModeController::modeChanged,
            this, &MainWindow::updateToolRailForMode);

    setupCloudAccountStatusControl();
    updateToolRailForMode();

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
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, &MainWindow::updateToolRailForMode);
    connect(MeshValidator::instance(), &MeshValidator::selectionChanged,
            this, &MainWindow::updateToolRailForMode);

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

    // Asset Browser toggle — use the dock's own action so tabified bottom
    // docks behave consistently with Dope Sheet / Curve Editor.
    if (m_assetBrowserDock && ui->menuView) {
        QAction* assetAct = m_assetBrowserDock->toggleViewAction();
        assetAct->setText(tr("Asset Browser"));
        ui->menuView->insertAction(ui->actionAsset_Browser, assetAct);
        ui->menuView->removeAction(ui->actionAsset_Browser);

        connect(ui->actionAsset_Browser, &QAction::toggled, this, [assetAct](bool checked) {
            if (assetAct->isChecked() != checked)
                assetAct->trigger();
        });
        connect(assetAct, &QAction::toggled, ui->actionAsset_Browser, &QAction::setChecked);
        connect(assetAct, &QAction::triggered, this, [this](bool checked) {
            SentryReporter::addBreadcrumb("ui.action",
                checked ? "Asset Browser shown" : "Asset Browser hidden");
            if (!checked || !m_assetBrowserDock)
                return;

            QTimer::singleShot(0, this, [this]() {
                showBottomToolDock(m_assetBrowserDock);
            });
        });
    }

    // Dope Sheet toggle — uses the dock's own toggleViewAction so we don't
    // have to add a new entry to mainwindow.ui.
    if (m_dopeSheetDock && ui->menuView) {
        QAction* dopeAct = m_dopeSheetDock->toggleViewAction();
        dopeAct->setText(tr("Dope Sheet"));
        ui->menuView->addAction(dopeAct);
        connect(dopeAct, &QAction::triggered, this, [this](bool checked) {
            if (!checked || !m_dopeSheetDock)
                return;

            QTimer::singleShot(0, this, [this]() {
                showBottomToolDock(m_dopeSheetDock);
            });
        });
    }
    // Curve Editor toggle — same pattern, lives next to Dope Sheet.
    if (m_curveEditorDock && ui->menuView) {
        QAction* curveAct = m_curveEditorDock->toggleViewAction();
        curveAct->setText(tr("Curve Editor"));
        ui->menuView->addAction(curveAct);
        connect(curveAct, &QAction::triggered, this, [this](bool checked) {
            if (!checked || !m_curveEditorDock)
                return;

            QTimer::singleShot(0, this, [this]() {
                showBottomToolDock(m_curveEditorDock);
            });
        });
    }
    if (ui->menuView) {
        auto* uvAct = new QAction(tr("UV Editor Window…"), this);
        uvAct->setObjectName(QStringLiteral("actionView_UV_Editor_Window"));
        ui->menuView->addAction(uvAct);
        connect(uvAct, &QAction::triggered, this, []() {
            SentryReporter::addBreadcrumb("ui.action", "View: UV Editor Window");
            EditorModeController::instance()->setCurrentMode(
                EditorModeController::MaterialMode);
            UVEditorController::instance()->openEditorWindow();
        });
    }
    if (m_bottomContextDock && m_consoleDock && ui->menuView) {
        m_contextPanelViewAction = new QAction(tr("Context Panel"), this);
        m_contextPanelViewAction->setObjectName(QStringLiteral("actionView_Context_Panel"));
        m_contextPanelViewAction->setCheckable(true);
        m_contextPanelViewAction->setChecked(
            QSettings().value(QStringLiteral("View/showContextPanel"), true).toBool());
        ui->menuView->addAction(m_contextPanelViewAction);
        connect(m_contextPanelViewAction, &QAction::toggled, this, [this](bool on) {
            QSettings().setValue(QStringLiteral("View/showContextPanel"), on);
            SentryReporter::addBreadcrumb(
                QStringLiteral("ui.action"),
                on ? QStringLiteral("View: Context Panel enabled")
                   : QStringLiteral("View: Context Panel disabled"));
            if (!m_bottomContextDock)
                return;
            if (on)
                showBottomToolDock(m_bottomContextDock);
            else
                m_bottomContextDock->hide();
        });

        m_consoleViewAction = new QAction(tr("Console"), this);
        m_consoleViewAction->setObjectName(QStringLiteral("actionView_Console"));
        m_consoleViewAction->setCheckable(true);
        m_consoleViewAction->setChecked(
            QSettings().value(QStringLiteral("View/showConsole"), true).toBool());
        ui->menuView->addAction(m_consoleViewAction);
        connect(m_consoleViewAction, &QAction::toggled, this, [this](bool on) {
            QSettings().setValue(QStringLiteral("View/showConsole"), on);
            SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                on ? QStringLiteral("View: Console enabled")
                   : QStringLiteral("View: Console disabled"));
            if (!m_consoleDock)
                return;
            if (on)
                showBottomToolDock(m_consoleDock);
            else
                m_consoleDock->hide();
        });
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
    for (EditorViewport* vp : mDockWidgetList) {
        connect(vp->getOgreWidget(), &OgreWidget::focusOnWidget,
                m_viewCubeController, &ViewCubeController::setActiveWidget);
    }

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
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("AI menu: Open AI Chat"));
        if (m_chatDock) {
            ensureLazyDockQml(m_chatDock);
            m_chatDock->show();
            m_chatDock->raise();
        }
    });
    aiMenu->addSeparator();
    QAction* aiSettingsAction = aiMenu->addAction(tr("AI Model Settings..."));
    connect(aiSettingsAction, &QAction::triggered, this, &MainWindow::showAIModelSettings);

    QAction* mcpSettingsAction = aiMenu->addAction(tr("MCP Server Settings..."));
    connect(mcpSettingsAction, &QAction::triggered, this, &MainWindow::showMCPSettings);

#ifdef ENABLE_AUTO_UPDATER
    ui->actionVerify_Update->setText(tr("Check for Updates..."));
#else
    ui->menuHelp->removeAction(ui->actionVerify_Update);
#endif

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

    // Send Feedback (#701)
    QAction* sendFeedbackAction = ui->menuHelp->addAction(tr("Send Feedback..."));
    sendFeedbackAction->setObjectName(QStringLiteral("actionSendFeedback"));
    connect(sendFeedbackAction, &QAction::triggered, this, [this]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Help > Send Feedback"));
        showSendFeedbackDialog({});
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

    // Initialize LLMManager after the window is up — starting the worker thread
    // during initToolBar competes with Ogre + QML startup on the main thread.
    QTimer::singleShot(0, this, []() { LLMManager::instance(); });

    // Image-to-3D (epic #764) lives in the Object-mode "Mode Tools" panel
    // (qml/PropertiesPanel.qml → MeshGenController), not a Tools-menu item.

#ifdef ENABLE_PS1_RIP
    QMenu *toolsMenu = menuBar()->addMenu(tr("&Tools"));
    toolsMenu->setObjectName(QStringLiteral("menuTools"));
    QMenu *experimentalMenu = toolsMenu->addMenu(tr("Experimental"));
    QAction *ps1RipAction = experimentalMenu->addAction(tr("PS1 Runtime Ripper…"));
    ps1RipAction->setObjectName(QStringLiteral("actionPS1RuntimeRipper"));
    connect(ps1RipAction, &QAction::triggered, this, []() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Tools > Experimental > PS1 Runtime Ripper"));
        PS1RipSessionWindow::showSession(nullptr);
    });
#endif
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

void MainWindow::setupCloudAccountStatusControl()
{
    m_cloudAccountControl = new CloudAccountMenuButton(this);

    connect(m_cloudAccountControl, &CloudAccountMenuButton::signInRequested, this,
            &MainWindow::signInToQtMeshCloud);
    connect(m_cloudAccountControl, &CloudAccountMenuButton::signOutRequested, this,
            &MainWindow::signOutOfQtMeshCloud);
    connect(m_cloudAccountControl, &CloudAccountMenuButton::uploadFilesRequested, this,
            &MainWindow::uploadFilesToQtMeshCloud);
    connect(m_cloudAccountControl, &CloudAccountMenuButton::openProjectsRequested, this,
            &MainWindow::showCloudProjectsDialog);
    connect(m_cloudAccountControl, &CloudAccountMenuButton::feedbackRequested, this, [this]() {
        showSendFeedbackDialog({});
    });

    // Push the account control to the bottom of the left objects toolbar (VS Code-style).
    auto* toolbarStretch = new QWidget(ui->objectsToolbar);
    toolbarStretch->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
    toolbarStretch->setMinimumSize(0, 0);
    QAction* stretchAction = ui->objectsToolbar->addWidget(toolbarStretch);
    stretchAction->setObjectName(QStringLiteral("modeAnyObjectsToolbarStretch"));

    QAction* cloudAction = ui->objectsToolbar->addWidget(m_cloudAccountControl);
    cloudAction->setObjectName(QStringLiteral("modeAnyCloudAccountAction"));
    updateCloudAuthActions();
    updateCloudUploadActionState();
}

void MainWindow::updateCloudAuthActions()
{
    if (m_cloudAccountControl)
        m_cloudAccountControl->refresh();
    updateCloudUploadActionState();
}

void MainWindow::updateCloudUploadActionState()
{
    const bool hasAsset = !primaryCloudAssetPath().isEmpty()
        || !Manager::getSingleton()->getEntities().isEmpty();
    if (m_cloudUploadMenuAction) {
        m_cloudUploadMenuAction->setEnabled(hasAsset);
        m_cloudUploadMenuAction->setToolTip(hasAsset
            ? tr("Upload the current asset and its dependencies to QtMesh Cloud")
            : tr("Open a model first"));
    }
    if (m_cloudAccountControl)
        m_cloudAccountControl->setUploadEnabled(hasAsset);
}

void MainWindow::showSendFeedbackDialog(const FeedbackPrefill& prefill)
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Send Feedback dialog opened"));

    FeedbackDialog dialog(this);
    dialog.applyPrefill(prefill);

    const auto refreshAccount = [&dialog]() {
        dialog.setSignedInAccountLabel(storedCloudDisplayName(), CloudCredentialStore::hasSession());
    };
    refreshAccount();

    dialog.signInRequested = [this, refreshAccount]() {
        signInToQtMeshCloud();
        refreshAccount();
    };

    dialog.exec();
}

void MainWindow::signInToQtMeshCloud()
{
    SentryReporter::addBreadcrumb(QStringLiteral("cloud.auth"),
                                  QStringLiteral("QtMesh Cloud sign-in started"));

    const auto code = QtMeshCloudClient::requestDeviceCode();
    if (!code.ok) {
        QMessageBox::warning(this, tr("QtMesh Cloud Sign In"),
                             tr("Could not start sign in.\n\n%1").arg(code.errorString));
        return;
    }

    QDialog prompt(this);
    prompt.setWindowTitle(tr("Sign in to QtMesh Cloud"));
    prompt.setWindowModality(Qt::WindowModal);
    auto* layout = new QVBoxLayout(&prompt);
    auto* label = new QLabel(
        tr("Enter this code on the QtMesh Cloud website, or click Open Browser.\n\n"
           "Code: %1")
            .arg(code.userCode),
        &prompt);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(label);

    auto* statusLabel = new QLabel(tr("Waiting for approval…"), &prompt);
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    auto* buttons = new QHBoxLayout();
    auto* copyButton = new QPushButton(tr("Copy Code"), &prompt);
    auto* openButton = new QPushButton(tr("Open Browser"), &prompt);
    auto* cancelButton = new QPushButton(tr("Cancel"), &prompt);
    buttons->addWidget(copyButton);
    buttons->addStretch();
    buttons->addWidget(cancelButton);
    buttons->addWidget(openButton);
    layout->addLayout(buttons);

    QTimer pollTimer(&prompt);
    bool signedIn = false;
    int intervalMs = qMax(1, code.intervalSeconds) * 1000;
    const int maxAttempts = qMax(1, code.expiresInSeconds / qMax(1, code.intervalSeconds) + 2);
    int attempts = 0;

    const auto applyToken = [&](const QtMeshCloudClient::DeviceTokenResult& token) -> bool {
        CloudSession session;
        session.token = token.token;
        session.expiresAt = token.expiresAt;
        session.email = token.user.value(QStringLiteral("email")).toString();
        if (!CloudCredentialStore::saveSession(session)) {
            QMessageBox::warning(this, tr("QtMesh Cloud Sign In"),
                                 tr("Signed in, but the session could not be saved securely."));
            return false;
        }

        QSettings settings;
        settings.setValue(AppSettingsKeys::cloudUserName(),
                          token.user.value(QStringLiteral("name")).toString());
        settings.setValue(AppSettingsKeys::cloudUserSlug(),
                          token.user.value(QStringLiteral("slug")).toString());
        // NB: do NOT remove the cloudToken / cloudTokenExpiresAt / cloudUserEmail
        // keys here. CloudCredentialStore now persists the session in exactly
        // those QSettings keys (it no longer uses the OS keychain), so removing
        // them would erase the session we just saved and log the user out on the
        // next launch.
        settings.sync();
        updateCloudAuthActions();
        return true;
    };

    const auto failSignIn = [&](const QString& message) {
        pollTimer.stop();
        if (!prompt.isVisible())
            return;
        QMessageBox::warning(this, tr("QtMesh Cloud Sign In"), message);
        prompt.reject();
    };

    const auto pollOnce = [&]() {
        if (attempts >= maxAttempts) {
            failSignIn(tr("The sign-in request expired. Start sign in again."));
            return;
        }
        ++attempts;

        const auto token = QtMeshCloudClient::pollDeviceToken(code.deviceCode);
        if (token.ok) {
            pollTimer.stop();
            if (applyToken(token)) {
                signedIn = true;
                prompt.accept();
            } else {
                prompt.reject();
            }
            return;
        }

        if (token.errorCode == QStringLiteral("authorization_pending")) {
            statusLabel->setText(tr("Waiting for approval…"));
            return;
        }
        if (token.errorCode == QStringLiteral("slow_down")) {
            intervalMs = qMax(intervalMs + 2000, qMax(1, token.intervalSeconds) * 1000);
            pollTimer.setInterval(intervalMs);
            statusLabel->setText(tr("Waiting for approval…"));
            return;
        }
        if (token.errorCode == QStringLiteral("access_denied")) {
            failSignIn(tr("Sign in was denied in the browser."));
            return;
        }
        if (token.errorCode == QStringLiteral("expired_token")) {
            failSignIn(tr("The sign-in code expired. Start sign in again."));
            return;
        }
        failSignIn(tr("Sign in failed.\n\n%1").arg(token.errorString));
    };

    connect(copyButton, &QPushButton::clicked, this, [userCode = code.userCode]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Cloud sign-in: Copy Code"));
        if (QApplication::clipboard())
            QApplication::clipboard()->setText(userCode);
    });
    connect(cancelButton, &QPushButton::clicked, &prompt, [&pollTimer, &prompt]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Cloud sign-in: Cancel"));
        pollTimer.stop();
        prompt.reject();
    });
    connect(openButton, &QPushButton::clicked, &prompt, [this, statusLabel, verificationUri = code.verificationUriComplete]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Cloud sign-in: Open Browser"));
        if (!QDesktopServices::openUrl(QUrl(verificationUri))) {
            SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                          QStringLiteral("Cloud sign-in: Open Browser failed"));
            QMessageBox::warning(this, tr("QtMesh Cloud"),
                                 tr("Could not open QtMesh Cloud in your browser."));
            return;
        }
        statusLabel->setText(tr("Complete sign-in in your browser…"));
    });
    connect(&pollTimer, &QTimer::timeout, pollOnce);

    openButton->setDefault(true);
    prompt.resize(420, prompt.sizeHint().height());

    pollTimer.start(intervalMs);
    QTimer::singleShot(0, &prompt, pollOnce);
    prompt.exec();
    pollTimer.stop();

    if (signedIn) {
        QMessageBox::information(this, tr("QtMesh Cloud Sign In"),
                                 tr("Signed in to QtMesh Cloud as %1.")
                                     .arg(storedCloudDisplayName()));
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.auth"),
                                      QStringLiteral("QtMesh Cloud sign-in completed"));
    }
}

void MainWindow::signOutOfQtMeshCloud()
{
    const QString token = CloudCredentialStore::loadSession().token;
    if (!token.isEmpty())
        QtMeshCloudClient::logout(token, /*timeoutMs=*/10000);

    CloudCredentialStore::clearSession();

    QSettings settings;
    settings.remove(AppSettingsKeys::cloudToken());
    settings.remove(AppSettingsKeys::cloudTokenExpiresAt());
    settings.remove(AppSettingsKeys::cloudUserName());
    settings.remove(AppSettingsKeys::cloudUserEmail());
    settings.remove(AppSettingsKeys::cloudUserSlug());
    settings.sync();
    if (m_cloudSession) {
        m_cloudSession->deleteLater();
        m_cloudSession = nullptr;
    }
    updateCloudAuthActions();
    SentryReporter::addBreadcrumb(QStringLiteral("cloud.auth"),
                                  QStringLiteral("QtMesh Cloud signed out"));
    QMessageBox::information(this, tr("QtMesh Cloud"), tr("Signed out of QtMesh Cloud."));
}

QtMeshCloudSession* MainWindow::cloudSessionForToken(const QString& token)
{
    if (m_cloudSession && m_cloudSession->bearerToken() != token) {
        m_cloudSession->deleteLater();
        m_cloudSession = nullptr;
    }
    if (!m_cloudSession)
        m_cloudSession = new QtMeshCloudSession(token, this);
    return m_cloudSession;
}

void MainWindow::showCloudProjectsDialog()
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("My Cloud Projects dialog requested"));

    CloudCredentialStore::migrateLegacySettingsIfNeeded();
    if (!CloudCredentialStore::hasSession()) {
        const int choice = QMessageBox::question(
            this,
            tr("QtMesh Cloud"),
            tr("Sign in to QtMesh Cloud to view your projects?"));
        if (choice != QMessageBox::Yes)
            return;
        signInToQtMeshCloud();
        if (!CloudCredentialStore::hasSession())
            return;
    }

    openCloudProjectsQmlDialog();
}

void MainWindow::openCloudProjectsQmlDialog(const QString& ownerSlug, const QString& projectSlug)
{
    if (m_cloudProjectsWindow) {
        if (auto* window = qobject_cast<QQuickWindow*>(m_cloudProjectsWindow)) {
            QMetaObject::invokeMethod(window, "open", Q_ARG(QVariant, ownerSlug),
                                      Q_ARG(QVariant, projectSlug));
            window->show();
            window->raise();
            window->requestActivate();
        }
        return;
    }

    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    auto* engine = new QQmlApplicationEngine(this);
    m_cloudProjectsEngine = engine;
    engine->addImportPath(QStringLiteral("qrc:/"));
    engine->addImportPath(QLibraryInfo::path(QLibraryInfo::QmlImportsPath));

    qmlRegisterSingletonType<PropertiesPanelController>(
        "PropertiesPanel", 1, 0, "PropertiesPanelController",
        [](QQmlEngine* eng, QJSEngine*) -> QObject* {
            return PropertiesPanelController::qmlInstance(eng, nullptr);
        });
    qmlRegisterSingletonType<CloudProjectsController>(
        "CloudProjects", 1, 0, "CloudProjectsController",
        [](QQmlEngine* eng, QJSEngine*) -> QObject* {
            return CloudProjectsController::qmlInstance(eng, nullptr);
        });

    connect(engine, &QQmlApplicationEngine::objectCreated, this,
            [this, engine, ownerSlug, projectSlug](QObject* obj, const QUrl&) {
                if (!obj) {
                    engine->deleteLater();
                    m_cloudProjectsEngine = nullptr;
                    return;
                }

                m_cloudProjectsWindow = obj;
                if (auto* window = qobject_cast<QQuickWindow*>(obj)) {
                    connect(window, &QQuickWindow::visibleChanged, this,
                            [this, window, engine](bool visible) {
                                if (visible || m_cloudProjectsWindow != window)
                                    return;
                                m_cloudProjectsWindow = nullptr;
                                m_cloudProjectsEngine = nullptr;
                                engine->deleteLater();
                            });

                    QMetaObject::invokeMethod(window, "open", Q_ARG(QVariant, ownerSlug),
                                              Q_ARG(QVariant, projectSlug));
                    window->show();
                    window->raise();
                    window->requestActivate();
                }
            });

    engine->load(QUrl(QStringLiteral("qrc:/CloudProjects/CloudProjectsDialog.qml")));
}

void MainWindow::closeCloudProjectsQmlDialog()
{
    CloudProjectsController::instance()->closeProjectFiles();
    if (auto* window = qobject_cast<QQuickWindow*>(m_cloudProjectsWindow))
        window->close();
}

void MainWindow::uploadFilesToQtMeshCloud()
{
    SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
                                  QStringLiteral("QtMesh Cloud upload requested"));

    CloudCredentialStore::migrateLegacySettingsIfNeeded();
    if (!CloudCredentialStore::hasSession()) {
        const int choice = QMessageBox::question(
            this,
            tr("QtMesh Cloud Upload"),
            tr("Sign in to QtMesh Cloud before uploading?"));
        if (choice != QMessageBox::Yes)
            return;
        signInToQtMeshCloud();
        if (!CloudCredentialStore::hasSession())
            return;
    }

    const QString mainAssetPath = primaryCloudAssetPath();
    if (mainAssetPath.isEmpty()) {
        QMessageBox::information(this, tr("QtMesh Cloud Upload"),
                                 tr("Open a model first, then upload it with its dependencies."));
        return;
    }

    const QString token = CloudCredentialStore::loadSession().token;
    if (token.isEmpty()) {
        QMessageBox::warning(this, tr("QtMesh Cloud Upload"),
                             tr("The saved QtMesh Cloud session is empty. Sign in again."));
        updateCloudAuthActions();
        return;
    }

    if (m_cloudUploadListingInFlight) {
        statusBar()->showMessage(tr("Cloud upload already in progress…"), 3000);
        return;
    }
    m_cloudUploadListingInFlight = true;

    statusBar()->showMessage(tr("Loading cloud projects…"), 0);
    m_cloudUploadProgress->start(tr("Loading cloud projects…"), 1);

    QtMeshCloudSession* session = cloudSessionForToken(token);
    disconnect(session, &QtMeshCloudSession::projectsListed, this, nullptr);
    QPointer<MainWindow> self(this);
    connect(session, &QtMeshCloudSession::projectsListed, this,
            [self, token, mainAssetPath](const QList<QtMeshCloudClient::ProjectSummary>& projects,
                                         const QString& listError, const QString&, bool) {
                if (!self)
                    return;

                self->m_cloudUploadListingInFlight = false;
                self->m_cloudUploadProgress->hideProgress();
                self->statusBar()->clearMessage();

                if (!listError.isEmpty()) {
                    QMessageBox::warning(self, self->tr("QtMesh Cloud Upload"),
                                         self->tr("Could not load your cloud projects.\n\n%1")
                                             .arg(listError));
                    return;
                }
                if (projects.isEmpty()) {
                    QMessageBox::information(
                        self, self->tr("QtMesh Cloud Upload"),
                        self->tr("You do not have any cloud projects yet. Create a project on "
                                 "QtMesh Cloud first, then upload files into it."));
                    return;
                }

                if (!CloudCredentialStore::hasSession()) {
                    QMessageBox::warning(self, self->tr("QtMesh Cloud Upload"),
                                         self->tr("Your QtMesh Cloud session expired. Sign in again."));
                    self->updateCloudAuthActions();
                    return;
                }
                const QString currentToken = CloudCredentialStore::loadSession().token;
                if (currentToken.isEmpty() || currentToken != token) {
                    QMessageBox::warning(self, self->tr("QtMesh Cloud Upload"),
                                         self->tr("Your QtMesh Cloud session changed while loading "
                                                  "projects. Sign in again and retry."));
                    self->updateCloudAuthActions();
                    return;
                }

                CloudUploadDialog dialog(self);
                dialog.setAccountLabel(storedCloudDisplayName());
                dialog.setProjects(projects);
                dialog.setMainAssetPath(mainAssetPath);

                if (dialog.exec() != QDialog::Accepted)
                    return;

                if (!dialog.hasSelectedProject()) {
                    QMessageBox::warning(self, self->tr("QtMesh Cloud Upload"),
                                         self->tr("Select a cloud project."));
                    return;
                }

                const QtMeshCloudClient::ProjectSummary selectedProject = dialog.selectedProject();

                CloudPackageUploadRequest request;
                request.mainAssetPath = mainAssetPath;
                request.selectedAbsolutePaths = dialog.selectedAbsolutePathsForUpload();
                request.projectName = dialog.projectName();
                request.ownerSlug = selectedProject.ownerSlug;
                request.projectSlug = selectedProject.projectSlug;
                request.createNewProject = false;
                request.runLocalScan = dialog.runLocalScanBeforeUpload();

                self->startCloudPackageUpload(self->cloudSessionForToken(currentToken), request);
            },
            Qt::SingleShotConnection);

    session->listProjects();
}

void MainWindow::startCloudPackageUpload(QtMeshCloudSession* session,
                                         const CloudPackageUploadRequest& request)
{
    if (!session)
        return;

    disconnect(session, &QtMeshCloudSession::uploadProgress, this, nullptr);
    disconnect(session, &QtMeshCloudSession::uploadFinished, this, nullptr);
    disconnect(session, &QtMeshCloudSession::uploadCanceled, this, nullptr);
    disconnect(session, &QtMeshCloudSession::uploadPrepareWarning, this, nullptr);
    disconnect(m_cloudUploadProgress, &CloudUploadProgress::cancelRequested, session, nullptr);

    m_cloudUploadProgress->start(tr("Preparing QtMesh Cloud upload…"), 1);

    connect(session, &QtMeshCloudSession::uploadProgress, this,
            [this](int current, int total, const QString& fileName) {
                m_cloudUploadProgress->updateProgress(current, total, fileName);
            });
    connect(session, &QtMeshCloudSession::uploadPrepareWarning, this,
            [this](const QString& warning) {
                if (!warning.isEmpty())
                    statusBar()->showMessage(warning, 8000);
            });
    connect(m_cloudUploadProgress, &CloudUploadProgress::cancelRequested, session,
            &QtMeshCloudSession::cancel);

    connect(session, &QtMeshCloudSession::uploadCanceled, this, [this]() {
        m_cloudUploadProgress->finish(false, tr("Upload canceled"));
        QTimer::singleShot(3000, m_cloudUploadProgress, &CloudUploadProgress::hideProgress);
    }, Qt::SingleShotConnection);

    connect(session, &QtMeshCloudSession::uploadFinished, this,
            [this](bool ok, const QString& error, const QString& projectUrl,
                   const QString& scanStatus) {
                if (!ok) {
                    m_cloudUploadProgress->finish(false, tr("Upload failed"));
                    QMessageBox::warning(this, tr("QtMesh Cloud Upload"),
                                         tr("Upload failed.\n\n%1").arg(error));
                    QTimer::singleShot(3000, m_cloudUploadProgress, &CloudUploadProgress::hideProgress);
                    return;
                }

                m_cloudUploadProgress->finish(true, tr("Upload complete"));
                statusBar()->showMessage(tr("Uploaded to QtMesh Cloud."), 5000);

                QMessageBox done(this);
                if (!error.isEmpty()) {
                    done.setIcon(QMessageBox::Warning);
                } else {
                    done.setIcon(QMessageBox::Information);
                }
                done.setWindowTitle(tr("QtMesh Cloud Upload"));
                done.setText(tr("Upload to QtMesh Cloud completed."));
                QString infoText;
                if (!error.isEmpty())
                    infoText = error;
                else if (!scanStatus.isEmpty())
                    infoText = tr("Scan status: %1").arg(scanStatus);
                if (!infoText.isEmpty())
                    done.setInformativeText(infoText);
                QPushButton* openButton = nullptr;
                QPushButton* copyButton = nullptr;
                if (!projectUrl.isEmpty()) {
                    openButton = done.addButton(tr("Open in Browser"), QMessageBox::AcceptRole);
                    copyButton = done.addButton(tr("Copy Link"), QMessageBox::ActionRole);
                }
                done.addButton(QMessageBox::Ok);
                done.exec();
                if (openButton && done.clickedButton() == openButton)
                    QDesktopServices::openUrl(QUrl(projectUrl));
                if (copyButton && done.clickedButton() == copyButton && QApplication::clipboard())
                    QApplication::clipboard()->setText(projectUrl);

                QTimer::singleShot(3000, m_cloudUploadProgress, &CloudUploadProgress::hideProgress);
            },
            Qt::SingleShotConnection);

    session->uploadPackageFromAssets(request);
}

void MainWindow::setPlaying(bool playing)
{   isPlaying = playing;    }

void MainWindow::createModeSurfaces()
{
    if (!m_modeBarShell) {
        m_modeBarShell = new QToolBar(tr("Modes"), this);
        m_modeBarShell->setObjectName("modeBarToolbar");
        m_modeBarShell->setMovable(false);
        m_modeBarShell->setFloatable(false);
        m_modeBarShell->setAllowedAreas(Qt::TopToolBarArea);

        m_modeBar = new QQuickWidget(m_modeBarShell);
        m_modeBar->setResizeMode(QQuickWidget::SizeRootObjectToView);
        m_modeBar->setMinimumHeight(38);
        m_modeBar->setMaximumHeight(38);
        // Pick a minimum width wide enough to fit all five mode buttons plus
        // the trailing Edit/Exit button without truncating labels. We derive
        // it from the current font metrics so HiDPI / accessibility-scaled
        // fonts don't end up clipped, with `kModeBarMinWidthFloor` as a
        // safety net for unusually narrow fonts.
        constexpr int kModeBarMinWidthFloor = 560;
        const QFontMetrics fm(m_modeBar->font());
        const int labelWidth = fm.horizontalAdvance(
            QStringLiteral("Object Edit Animation Material Validation Edit"));
        // Per-button chrome (border + padding + spacing) plus the Edit button
        // and the layout's left/right margins (~10 px each side).
        constexpr int kPerButtonChrome = 24;
        constexpr int kButtons = 6; // 5 modes + Edit toggle
        const int derivedWidth = labelWidth + kPerButtonChrome * kButtons + 40;
        m_modeBar->setMinimumWidth(qMax(kModeBarMinWidthFloor, derivedWidth));
        // Minimum width only — a stretch toolbar to the left absorbs extra space
        // so the mode bar stays on the right of the top row next to Tools.
        m_modeBar->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        m_modeBar->setSource(QUrl("qrc:/ModeBar/ModeBar.qml"));
        m_modeBarShell->addWidget(m_modeBar);

        m_topBarStretch = new QToolBar(this);
        m_topBarStretch->setObjectName(QStringLiteral("topToolBarStretch"));
        m_topBarStretch->setWindowTitle(QString());
        m_topBarStretch->setMovable(false);
        m_topBarStretch->setFloatable(false);
        m_topBarStretch->setAllowedAreas(Qt::TopToolBarArea);
        m_topBarStretch->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* stretch = new QWidget(m_topBarStretch);
        stretch->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        stretch->setMinimumWidth(0);
        m_topBarStretch->addWidget(stretch);
        addToolBar(Qt::TopToolBarArea, m_topBarStretch);
        addToolBar(Qt::TopToolBarArea, m_modeBarShell);
    }

    if (!m_bottomContextDock) {
        auto* contextWidget = new QQuickWidget();
        contextWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
        contextWidget->setMinimumHeight(kBottomToolHeight);
        contextWidget->setMaximumHeight(kBottomToolHeight);
        contextWidget->setSource(QUrl("qrc:/BottomContextPanel/BottomContextPanel.qml"));
        if (auto* root = contextWidget->rootObject()) {
            root->setProperty("materialEditorAction",
                              QVariant::fromValue(static_cast<QObject*>(ui->actionMaterial_Editor)));
        }

        m_bottomContextDock = new QDockWidget(tr("Context"), this);
        m_bottomContextDock->setObjectName("BottomContextDock");
        m_bottomContextDock->setAllowedAreas(Qt::BottomDockWidgetArea);
        m_bottomContextDock->setWidget(contextWidget);
        configureBottomToolDock(m_bottomContextDock);
        addDockWidget(Qt::BottomDockWidgetArea, m_bottomContextDock);
        resizeDocks({m_bottomContextDock}, {kBottomToolHeight}, Qt::Vertical);
        m_bottomContextDock->hide();
        connect(m_bottomContextDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
            if (!visible)
                return;

            QTimer::singleShot(0, this, [this]() {
                if (m_bottomContextDock)
                    m_bottomContextDock->raise();
            });
        });
    }
}

QAction* MainWindow::actionShowGrid() const
{
    return ui ? ui->actionShow_Grid : nullptr;
}

QAction* MainWindow::actionShowNormals() const
{
    return ui ? ui->actionShow_Normals : nullptr;
}

QAction* MainWindow::actionShowMeshInfo() const
{
    return ui ? ui->actionShow_Mesh_Info : nullptr;
}

QAction* MainWindow::actionShowViewCube() const
{
    return ui ? ui->actionShow_View_Cube : nullptr;
}

void MainWindow::revealBottomTool(const QString& toolId)
{
    QDockWidget* dock = nullptr;
    QString breadcrumb;
    if (toolId == QStringLiteral("context"))
    {
        dock = m_bottomContextDock;
        breadcrumb = QStringLiteral("Rail: Context");
    }
    else if (toolId == QStringLiteral("assetBrowser"))
    {
        dock = m_assetBrowserDock;
        breadcrumb = QStringLiteral("Rail: Asset Browser");
    }
    else if (toolId == QStringLiteral("dopeSheet"))
    {
        dock = m_dopeSheetDock;
        breadcrumb = QStringLiteral("Rail: Dope Sheet");
    }
    else if (toolId == QStringLiteral("curveEditor"))
    {
        dock = m_curveEditorDock;
        breadcrumb = QStringLiteral("Rail: Curve Editor");
    }
    else if (toolId == QStringLiteral("console"))
    {
        dock = m_consoleDock;
        breadcrumb = QStringLiteral("Rail: Console");
    }
    else if (toolId == QStringLiteral("uvEditor"))
    {
        SentryReporter::addBreadcrumb("ui.action", QStringLiteral("Rail: UV Editor"));
        EditorModeController::instance()->setCurrentMode(
            EditorModeController::MaterialMode);
        UVEditorController::instance()->openEditorWindow();
        return;
    }

    if (dock) {
        SentryReporter::addBreadcrumb("ui.action", breadcrumb);
        showBottomToolDock(dock);
    }
}

void MainWindow::appendConsoleLine(const QString& line)
{
    if (!m_consoleEdit)
        return;
    m_consoleEdit->appendPlainText(line);
    if (QScrollBar* sb = m_consoleEdit->verticalScrollBar())
        sb->setValue(sb->maximum());
}

void MainWindow::configureBottomToolDock(QDockWidget* dock)
{
    if (!dock)
        return;

    dock->setAllowedAreas(Qt::BottomDockWidgetArea);

    QWidget* content = dock->widget();
    if (content) {
        content->setMinimumHeight(kBottomToolHeight);
        content->setMaximumHeight(dock->isFloating() ? QWIDGETSIZE_MAX : kBottomToolHeight);
    }

    dock->setMaximumHeight(dock->isFloating() ? QWIDGETSIZE_MAX : kBottomDockMaxHeight);

    static const char connectedProperty[] = "_qtme_bottomToolSignalsConnected";
    if (dock->property(connectedProperty).toBool())
        return;

    dock->setProperty(connectedProperty, true);
    connect(dock, &QDockWidget::topLevelChanged, this, [this, dock](bool) {
        configureBottomToolDock(dock);
        if (!dock->isFloating()) {
            QTimer::singleShot(0, this, [this]() {
                tabifyBottomToolDocks();
            });
        }
    });
}

void MainWindow::showBottomToolDock(QDockWidget* dock)
{
    if (!dock)
        return;

    ensureLazyDockQml(dock);

    if (dock->isFloating())
        dock->setFloating(false);

    if (dockWidgetArea(dock) != Qt::BottomDockWidgetArea)
        addDockWidget(Qt::BottomDockWidgetArea, dock);

    configureBottomToolDock(dock);
    tabifyBottomToolDocks();
    dock->show();
    resizeDocks({dock}, {kBottomToolHeight}, Qt::Vertical);
    dock->raise();
}

void MainWindow::tabifyBottomToolDocks()
{
    // Bottom tool surfaces are meant to collapse into one tab group
    // (Context / Console / Asset Browser / Dope Sheet / Curve Editor), so ensure
    // the shell actually allows tabbed docking before we call
    // QMainWindow::tabifyDockWidget().
    if (!(dockOptions() & QMainWindow::AllowTabbedDocks))
        setDockOptions(dockOptions() | QMainWindow::AllowTabbedDocks);

    // Tab order (left-to-right): Context + Console first; animation/asset tools after.
    const QList<QDockWidget*> docks = {
        m_bottomContextDock,
        m_consoleDock,
        m_assetBrowserDock,
        m_dopeSheetDock,
        m_curveEditorDock
    };

    QDockWidget* anchor = nullptr;
    for (QDockWidget* dock : docks) {
        configureBottomToolDock(dock);
        if (!anchor && dock && !dock->isFloating() && dockWidgetArea(dock) == Qt::BottomDockWidgetArea)
            anchor = dock;
    }

    if (!anchor)
        return;

    const QList<QDockWidget*> anchorTabs = tabifiedDockWidgets(anchor);
    for (QDockWidget* dock : docks) {
        if (!dock || dock == anchor || dock->isFloating() || dockWidgetArea(dock) != Qt::BottomDockWidgetArea)
            continue;

        if (!anchorTabs.contains(dock) && !tabifiedDockWidgets(dock).contains(anchor))
            tabifyDockWidget(anchor, dock);
    }
}

void MainWindow::updateToolRailForMode()
{
    const int mode = EditorModeController::instance()->currentMode();
    const bool objectMode = mode == EditorModeController::ObjectMode;
    const bool editMode = mode == EditorModeController::EditMode;
    const bool animationMode = mode == EditorModeController::AnimationMode;
    const bool validationMode = mode == EditorModeController::ValidationMode;

    for (QAction* action : ui->objectsToolbar->actions()) {
        const QString name = action->objectName();
        if (name.startsWith(QStringLiteral("modeObject"))) {
            action->setVisible(objectMode);
        } else if (name.startsWith(QStringLiteral("modeEdit"))) {
            action->setVisible(editMode && EditModeController::instance()->isEditModeActive());
        } else if (name.startsWith(QStringLiteral("modeAnimation"))) {
            action->setVisible(animationMode);
        } else if (name.startsWith(QStringLiteral("modeValidation"))) {
            action->setVisible(validationMode);
        } else if (name.startsWith(QStringLiteral("modeAny"))) {
            action->setVisible(true);
        }

        if (name == QStringLiteral("modeValidationRunAction"))
            action->setEnabled(MeshValidator::instance()->hasSelection());
    }

    // Top Tools bar: gizmo mode + space toggle stay visible in every editor mode.
    ui->actionSelect_Object->setVisible(true);
    ui->actionTranslate_Object->setVisible(true);
    ui->actionRotate_Object->setVisible(true);
    ui->actionScale_Object->setVisible(true);
    ui->actionToggle_Transform_Space->setVisible(true);
}

// LCOV_EXCL_START — Ogre frame listener requires render loop
bool MainWindow::frameStarted(const Ogre::FrameEvent &evt)
{    return true;   }

namespace {

// Advance every enabled animation state on `ent`. The selected clip on the
// active entity goes through advanceTime() (slice-A speed + loop region);
// everything else uses speed-scaled raw addTime().
void advanceEntityStates(Ogre::Entity* ent, bool isActiveEntity,
                         const std::string& activeAnim,
                         const AnimationControlController* animCtrl,
                         double dt, double scaledDt)
{
    const auto* set = ent->getAllAnimationStates();
    if (!set) return;
    for (const auto& [key, value] : set->getAnimationStates()) {
        if (!value->getEnabled()) continue;
        if (isActiveEntity && key == activeAnim) {
            const auto   now  = static_cast<double>(value->getTimePosition());
            const double next = animCtrl->advanceTime(now, dt);
            value->setTimePosition(static_cast<float>(next));
        } else {
            value->addTime(static_cast<float>(scaledDt));
        }
    }
}

} // namespace

bool MainWindow::frameRenderingQueued(const Ogre::FrameEvent &evt)
{
    // Advance time for every entity that has enabled animation states.
    // Speed is global (scales dt for all states). The loop region applies only
    // to the entity+animation selected in the Animation Control panel.
    if (!isPlaying) return true;

    const auto* animCtrl = AnimationControlController::instance();
    auto*       blender  = AnimationBlender::instance();
    const std::string activeEntity = animCtrl->selectedEntityName().toStdString();
    const std::string activeAnim   = animCtrl->selectedAnimation().toStdString();
    const auto   dt       = static_cast<double>(evt.timeSinceLastFrame);
    const double scaledDt = dt * animCtrl->playbackSpeed();

    for (Ogre::SceneNode* node : Manager::getSingleton()->getSceneNodes()) {
        if (!node) continue;
        for (int i = 0; i < static_cast<int>(node->numAttachedObjects()); ++i) {
            Ogre::MovableObject* obj = node->getAttachedObject(i);
            if (!obj || obj->getMovableType() != "Entity") continue;

            auto* ent = static_cast<Ogre::Entity*>(obj);
            const bool isActiveEntity =
                (!activeEntity.empty() && ent->getName() == activeEntity);

            // The blender owns the active entity when active — it writes all
            // weights and advances time itself. Pass raw dt so its internal
            // advanceTime() can apply playback speed + loop region.
            if (isActiveEntity && blender->apply(ent, dt)) continue;
            advanceEntityStates(ent, isActiveEntity, activeAnim, animCtrl, dt, scaledDt);
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

    // The editor mode (e.g. "Object mode", "Edit mode (Vertex)") is rendered by
    // the permanent `m_editModeLabel` widget on the right side of the status
    // bar (see updateEditModeIndicator()). Don't append it here too — that
    // would duplicate the same string on the rolling left-hand message.

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
        EditorModeController::instance()->toggleObjectEditMode();
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
#ifdef ENABLE_PS1_RIP
    // PS1 Extracted Asset Browser → editor viewport drag-and-drop (#426).
    // The asset browser publishes its tile's mesh-index list under a custom
    // MIME type and the matching meshIndex list under a sibling type so the
    // promote path doesn't have to re-parse the assetId string. We dispatch
    // before the URL/file path branch so a drag that also carries a text
    // representation doesn't get mis-routed as a file import. Whole branch
    // is guarded by `ENABLE_PS1_RIP` because `PS1RipSessionWindow` lives in
    // the optional PS1 ripping subsystem (off on macOS CI builds).
    static constexpr auto kPs1RipMeshMime = "application/x-ps1rip-mesh";
    static constexpr auto kPs1RipMeshIndexMime = "application/x-ps1rip-meshindex";
    if (event->mimeData()->hasFormat(QString::fromLatin1(kPs1RipMeshMime))) {
        const QStringList assetIds =
            QString::fromUtf8(event->mimeData()->data(QString::fromLatin1(kPs1RipMeshMime)))
                .split(QLatin1Char(';'), Qt::SkipEmptyParts);
        const QStringList meshIndexes =
            QString::fromUtf8(event->mimeData()->data(QString::fromLatin1(kPs1RipMeshIndexMime)))
                .split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (int i = 0; i < assetIds.size(); ++i) {
            const int meshIndex = i < meshIndexes.size() ? meshIndexes.at(i).toInt() : -1;
            PS1RipSessionWindow::promoteUniqueMeshById(meshIndex, assetIds.at(i));
        }
        event->acceptProposedAction();
        return;
    }
#endif // ENABLE_PS1_RIP

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

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    return QMainWindow::eventFilter(watched, event);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
// LCOV_EXCL_START — opens QFileDialog
void MainWindow::on_actionImport_triggered()
{
    SentryReporter::addBreadcrumb("ui.action", "Import mesh files");

    QStringList fileNames = QFileDialog::getOpenFileNames(this, tr("Select a mesh file to import"),
                                                     "",
                                                     MeshImporterExporter::importFileDialogFilter(),
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
    updateCloudUploadActionState();
}

void MainWindow::openLaunchFiles(const QStringList& paths)
{
    if (paths.isEmpty())
        return;

    show();
    raise();
    activateWindow();

    for (const QString& path : paths) {
        CloudDeepLinkTarget cloud;
        if (CloudDeepLink::decodeLaunchToken(path, &cloud)) {
            openCloudProjectFromDeepLink(cloud.ownerSlug, cloud.projectSlug);
            continue;
        }
        SentryReporter::addBreadcrumb(QStringLiteral("app.launch.file_open"),
                                      QFileInfo(path).fileName());
        loadFile(path);
    }
}

void MainWindow::openCloudProjectFromDeepLink(const QString& ownerSlug, const QString& projectSlug)
{
    show();
    raise();
    activateWindow();

    if (!CloudCredentialStore::hasSession()) {
        QMessageBox::information(this,
                                 tr("Sign in to QtMesh Cloud"),
                                 tr("Sign in to your QtMesh Cloud account to open %1/%2.")
                                     .arg(ownerSlug, projectSlug));
        signInToQtMeshCloud();
        if (!CloudCredentialStore::hasSession())
            return;
    }

    openCloudProjectsQmlDialog(ownerSlug, projectSlug);
}

void MainWindow::importCloudDownloadedFile(const QString& localMainFile)
{
    const QFileInfo fileInfo(localMainFile);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        QMessageBox::warning(this,
                             tr("QtMesh Cloud"),
                             tr("Downloaded project file was not found on disk."));
        return;
    }
    if (!AppLaunchHandler::isImportableMeshPath(localMainFile)) {
        QMessageBox::warning(this,
                             tr("QtMesh Cloud"),
                             tr("The downloaded project does not contain a supported mesh file."));
        return;
    }

    addToRecentFiles(localMainFile);
    if (m_welcomeController && m_welcomeController->isVisible())
        m_welcomeController->setVisible(false);

    MeshImporterExporter::prepareCloudCachedImport(localMainFile);

    QSet<QString> entityNamesBefore;
    for (auto* obj : Manager::getSingleton()->getEntities()) {
        if (obj && obj->getMovableType() == QLatin1String("Entity"))
            entityNamesBefore.insert(QString::fromStdString(obj->getName()));
    }

    auto countEntities = []() {
        int count = 0;
        for (auto* obj : Manager::getSingleton()->getEntities()) {
            if (obj && obj->getMovableType() == QLatin1String("Entity"))
                ++count;
        }
        return count;
    };

    const int entitiesBefore = countEntities();
    try {
        importMeshs({localMainFile});
    } catch (...) {
        QMessageBox::warning(this,
                             tr("QtMesh Cloud"),
                             tr("Could not import the downloaded project."));
        throw;
    }

    if (countEntities() == entitiesBefore) {
        QMessageBox::warning(this,
                             tr("QtMesh Cloud"),
                             tr("Download finished, but the mesh could not be imported."));
        return;
    }

    for (auto* obj : Manager::getSingleton()->getEntities()) {
        if (!obj || obj->getMovableType() != QLatin1String("Entity"))
            continue;
        if (entityNamesBefore.contains(QString::fromStdString(obj->getName())))
            continue;

        auto* entity = static_cast<Ogre::Entity*>(obj);
        MeshImporterExporter::rebindEntityMaterials(
            entity, MeshImporterExporter::textureSearchRootsForImportFile(localMainFile));
    }

    SpaceCamera* cam = nullptr;
    for (EditorViewport* vp : mDockWidgetList) {
        if (vp->getOgreWidget()->hasFocus()) {
            cam = vp->getOgreWidget()->getSpaceCamera();
            break;
        }
    }
    if (!cam && !mDockWidgetList.isEmpty())
        cam = mDockWidgetList.first()->getOgreWidget()->getSpaceCamera();
    if (cam)
        cam->frameSelection();

    QTimer::singleShot(0, this, [this, localMainFile, entityNamesBefore]() {
        const QStringList textureRoots =
            MeshImporterExporter::textureSearchRootsForImportFile(localMainFile);
        for (auto* obj : Manager::getSingleton()->getEntities()) {
            if (!obj || obj->getMovableType() != QLatin1String("Entity"))
                continue;
            if (entityNamesBefore.contains(QString::fromStdString(obj->getName())))
                continue;

            MeshImporterExporter::rebindEntityMaterials(static_cast<Ogre::Entity*>(obj), textureRoots);
        }

        if (m_pRoot && m_pRoot->getRenderSystem()) {
            try {
                m_pRoot->renderOneFrame();
            } catch (...) {
            }
        }
        for (EditorViewport* vp : mDockWidgetList)
            vp->getOgreWidget()->update();
    });
}

void MainWindow::importMeshs(const QStringList &_uriList)
{
    QSet<QString> entityNamesBefore;
    for (auto* obj : Manager::getSingleton()->getEntities()) {
        if (obj && obj->getMovableType() == QLatin1String("Entity"))
            entityNamesBefore.insert(QString::fromStdString(obj->getName()));
    }

    auto txn = SentryReporter::startTransaction("ui.import", "file.import");
    QList<Ogre::SkeletonPtr> animOnlySkeletons;
    try {
        MeshImporterExporter::importer(_uriList, 0, &animOnlySkeletons);
    } catch (...) {
        SentryReporter::finishTransaction(txn);
        throw;
    }
    SentryReporter::finishTransaction(txn);

    // Material rebinding (texture hydration + per-material RTSS sync + a
    // whole-scene material walk) is deferred to the next event-loop tick so it
    // doesn't block the import call and freeze the UI. It used to run here
    // (immediately) AND again in the QTimer below — doing the expensive work
    // TWICE per imported entity. Now it runs once, deferred.
    QTimer::singleShot(0, this, [this, entityNamesBefore]() {
        for (auto* obj : Manager::getSingleton()->getEntities()) {
            if (!obj || obj->getMovableType() != QLatin1String("Entity"))
                continue;
            if (entityNamesBefore.contains(QString::fromStdString(obj->getName())))
                continue;

            auto* entity = static_cast<Ogre::Entity*>(obj);
            MeshImporterExporter::rebindEntityMaterials(
                entity, MeshImporterExporter::textureSearchRootsForEntity(entity));
        }

        if (m_pRoot && m_pRoot->getRenderSystem()) {
            try {
                m_pRoot->renderOneFrame();
            } catch (...) {
            }
        }
        for (EditorViewport* vp : mDockWidgetList)
            vp->getOgreWidget()->update();
    });

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
        if (!MeshImporterExporter::sceneImporter(fileName)) {
            FeedbackReportHelper::showFailureWithReportOption(
                this, tr("Open Scene"), tr("Could not import scene file."),
                FeedbackReportHelper::importFailurePrefill(
                    QFileInfo(fileName).suffix(), tr("Could not import scene file.")));
            SentryReporter::finishTransaction(txn);
            return;
        }
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
        if (result != 0) {
            FeedbackReportHelper::showFailureWithReportOption(
                this, tr("Save Scene"), tr("Failed to save scene."),
                FeedbackReportHelper::exportFailurePrefill(
                    QFileInfo(fileName).suffix(), tr("Failed to save scene."),
                    QString::number(result)));
        }
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

    // Stop EVERY timer that could mutate the mesh / skeleton state
    // while a nested event loop is open (QFileDialog) OR while the
    // serializer walks the buffers. Qt fires queued timers inside
    // nested event loops, so without these pauses:
    //   * `MainWindow::m_pTimer` keeps calling renderOneFrame() — the
    //     animation state advances mid-export, leaving the SkeletonInstance
    //     pointers stale to whatever the serializer is reading.
    //   * `AnimationControlController::m_pollTimer` (60fps) calls
    //     `setAnimationFrame()`, which mutates AnimationState's
    //     time position and re-applies skeleton transforms — same race.
    //
    // FBX / PS1 exporters happened to survive because they snapshot
    // their inputs into temp buffers up front; Ogre's MeshSerializer
    // and Assimp's exporters walk the live mesh in place and crash.
    //
    // See #681 export-crash repro.
    const bool wasRendering = m_pTimer && m_pTimer->isActive();
    if (wasRendering) m_pTimer->stop();
    AnimationControlController::instance()->suspendPollTimer();

    try {
        const auto* sel = SelectionSet::getSingleton();

        if(sel->hasNodes())
        {
            foreach(Ogre::SceneNode* node, sel->getNodesSelectionList())
            {
                QString exportedPath = MeshImporterExporter::exporter(node, this);
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
                QString exportedPath = MeshImporterExporter::exporter(node, this);
                if (!exportedPath.isEmpty())
                    addToRecentFiles(exportedPath);
            }
        }
    } catch (...) {
        AnimationControlController::instance()->resumePollTimer();
        if (wasRendering) m_pTimer->start();
        SentryReporter::finishTransaction(txn);
        throw;
    }
    AnimationControlController::instance()->resumePollTimer();
    if (wasRendering) m_pTimer->start();
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
    // SelectionSet::selectionChanged is wired to updateMergeAnimationsButton
    // first and updateToolRailForMode immediately after (see the connect()
    // pair where these slots are registered). Qt fires connections in
    // registration order, so the tool rail is always refreshed for free
    // after this slot completes — calling updateToolRailForMode() here
    // would just iterate every toolbar action twice per selection event.
    const auto* sel = SelectionSet::getSingleton();
    const bool enough =
        sel->getNodesCount() + sel->getEntitiesCount() >= 2;
    ui->actionMerge_Animations->setEnabled(enough);
}

void MainWindow::triggerMergeAnimations()
{
    on_actionMerge_Animations_triggered();
}

ViewportCameraSnapshot MainWindow::queryViewportCamera(bool requireFocus) const
{
    ViewportCameraSnapshot snap;
    SpaceCamera* spaceCam = nullptr;
    bool focused = false;

    for (EditorViewport* vp : mDockWidgetList) {
        if (!vp || !vp->getOgreWidget())
            continue;
        if (vp->getOgreWidget()->hasFocus()) {
            spaceCam = vp->getOgreWidget()->getSpaceCamera();
            focused = true;
            break;
        }
    }

    // UV/material panels steal Qt focus — fall back to the last viewport the
    // user interacted with (same source as the transform gizmo / view cube).
    if (!spaceCam) {
        if (OgreWidget* active = TransformOperator::getSingleton()->getActiveWidget())
            spaceCam = active->getSpaceCamera();
    }

    if (!spaceCam && !requireFocus) {
        for (EditorViewport* vp : mDockWidgetList) {
            if (vp && vp->getOgreWidget()) {
                spaceCam = vp->getOgreWidget()->getSpaceCamera();
                if (spaceCam)
                    break;
            }
        }
    }

    if (!spaceCam || (requireFocus && !focused))
        return snap;

    Ogre::Camera* camera = spaceCam->getCamera();
    if (!camera)
        return snap;

    snap.viewMatrix = camera->getViewMatrix();
    snap.projMatrix = camera->getProjectionMatrix();
    snap.valid = true;
    snap.fromFocusedViewport = focused;
    return snap;
}

void MainWindow::triggerMaterialEditor()
{
    on_actionMaterial_Editor_triggered();
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

void MainWindow::on_actionMeshEditor_toggled(bool arg1)
{
    ui->meshEditorWidget->setVisible(arg1);
}

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
    auto* modeCtrl = EditorModeController::instance();
    auto* editCtrl = EditModeController::instance();
    if (editCtrl->isEditModeActive()) {
        m_editModeLabel->setText(modeCtrl->statusText());
        m_editModeLabel->setStyleSheet(
            "QLabel { font-weight: bold; padding: 2px 8px; "
            "background-color: #3d6b3d; color: white; border-radius: 3px; }");
    } else {
        m_editModeLabel->setText(modeCtrl->statusText());
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
        connect(pOgreViewport->getOgreWidget(), &OgreWidget::focusOnWidget,
                m_viewCubeController, &ViewCubeController::setActiveWidget);

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

// LCOV_EXCL_START — updater dialog
#ifdef ENABLE_AUTO_UPDATER
void MainWindow::showUpdaterDialog(bool runCheck)
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Help > Check for Updates"));

    if (m_updaterWindow) {
        if (auto* window = qobject_cast<QQuickWindow*>(m_updaterWindow)) {
            QMetaObject::invokeMethod(window, "open", Q_ARG(QVariant, runCheck));
            window->show();
            window->raise();
            window->requestActivate();
        }
        return;
    }

    auto* engine = new QQmlApplicationEngine(this);
    m_updaterEngine = engine;
    engine->addImportPath(QLibraryInfo::path(QLibraryInfo::QmlImportsPath));

    qmlRegisterSingletonType<PropertiesPanelController>(
        "PropertiesPanel", 1, 0, "PropertiesPanelController",
        [](QQmlEngine* eng, QJSEngine*) -> QObject* {
            return PropertiesPanelController::qmlInstance(eng, nullptr);
        });
    qmlRegisterSingletonType<UpdaterController>(
        "Updater", 1, 0, "UpdaterController",
        [](QQmlEngine* eng, QJSEngine*) -> QObject* {
            return UpdaterController::qmlInstance(eng, nullptr);
        });

    connect(engine, &QQmlApplicationEngine::objectCreated, this,
            [this, engine, runCheck](QObject* obj, const QUrl&) {
                if (!obj) {
                    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                                  QStringLiteral("Updater dialog: QML load failed"));
                    engine->deleteLater();
                    m_updaterEngine = nullptr;
                    return;
                }

                m_updaterWindow = obj;
                if (auto* window = qobject_cast<QQuickWindow*>(obj)) {
                    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
                    connect(window, &QQuickWindow::visibleChanged, this,
                            [this, window, engine](bool visible) {
                                if (visible || m_updaterWindow != window) {
                                    return;
                                }
                                m_updaterWindow = nullptr;
                                m_updaterEngine = nullptr;
                                engine->deleteLater();
                            });
                    QMetaObject::invokeMethod(window, "open", Q_ARG(QVariant, runCheck));
                    window->show();
                    window->raise();
                    window->requestActivate();
                }
            });

    engine->load(QUrl(QStringLiteral("qrc:/UpdaterDialog/UpdaterDialog.qml")));
}

void MainWindow::showUpdateToast(const QString& version)
{
    UpdaterTelemetry::breadcrumb(QStringLiteral("updater.background.toast"),
                                 QStringLiteral("version=%1").arg(version));

    if (m_updateToastEngine) {
        if (auto* toast = m_updateToastWindow) {
            QMetaObject::invokeMethod(toast, "showForVersion", Q_ARG(QVariant, version));
        }
        return;
    }

    m_updateToastEngine = new QQmlApplicationEngine(this);
    m_updateToastEngine->addImportPath(QStringLiteral("qrc:/"));
    qmlRegisterSingletonType<PropertiesPanelController>(
        "PropertiesPanel", 1, 0, "PropertiesPanelController",
        [](QQmlEngine* eng, QJSEngine*) -> QObject* {
            return PropertiesPanelController::qmlInstance(eng, nullptr);
        });
    qmlRegisterSingletonType<UpdaterController>(
        "Updater", 1, 0, "UpdaterController",
        [](QQmlEngine* eng, QJSEngine*) -> QObject* {
            return UpdaterController::qmlInstance(eng, nullptr);
        });

    connect(m_updateToastEngine, &QQmlApplicationEngine::objectCreated, this,
            [this, version](QObject* obj, const QUrl&) {
                if (!obj) {
                    m_updateToastEngine->deleteLater();
                    m_updateToastEngine = nullptr;
                    return;
                }

                m_updateToastWindow = obj;
                if (auto* window = qobject_cast<QQuickWindow*>(obj)) {
                    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
                    connect(window, &QQuickWindow::destroyed, this, [this]() {
                        m_updateToastWindow = nullptr;
                        if (m_updateToastEngine) {
                            m_updateToastEngine->deleteLater();
                            m_updateToastEngine = nullptr;
                        }
                    });
                }
                QMetaObject::invokeMethod(obj, "showForVersion", Q_ARG(QVariant, version));
            });

    m_updateToastEngine->load(QUrl(QStringLiteral("qrc:/UpdateToast/UpdateToast.qml")));
}

void MainWindow::on_actionVerify_Update_triggered()
{
    showUpdaterDialog(true);
}
#else
void MainWindow::on_actionVerify_Update_triggered()
{
}
#endif
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
