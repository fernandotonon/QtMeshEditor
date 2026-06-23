#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QComboBox>
#include <QMenu>
#include <QTableWidget>
#include <QColorDialog>
#include <OgreFrameListener.h>
#include <QNetworkAccessManager>

#include "TransformOperator.h"
#include "FeedbackPrefill.h"

class LLMSettingsWidget;
class MCPServer;
class NormalVisualizer;
class MeshInfoOverlay;
class ViewCubeController;
class PropertiesPanelController;
class EditModeController;
class EditorModeController;
class WelcomeScreenController;
class AssetBrowserController;
class QQuickWidget;
class QQmlApplicationEngine;
class QPlainTextEdit;
class QLabel;
class QToolBar;
class QAction;
class QToolButton;
class CloudAccountMenuButton;
class CloudUploadProgress;
class QtMeshCloudSession;
struct CloudPackageUploadRequest;
class OgreWidget;

namespace Ui {
class MainWindow;
}
class EditorViewport;
class PrimitivesWidget;

namespace Ogre
{
    class Root;
    class SceneNode;
    class RaySceneQuery;
    class ManualObject;
    class AnimationState;
}

class MainWindow : public QMainWindow, public Ogre::FrameListener
{
    Q_OBJECT

public:
    /// Pixel height applied to bottom-docked tool widgets (Context Panel,
    /// Console, Asset Browser, Dope Sheet, Curve Editor) when docked. Floating
    /// instances expand freely to QWIDGETSIZE_MAX.
    static constexpr int kDefaultDockedHeight = 180;
    /// Slightly larger than kDefaultDockedHeight so the dock title bar fits
    /// without forcing the inner content to shrink.
    static constexpr int kDefaultDockedMaxHeight = 220;

    explicit MainWindow(QWidget *parent = nullptr);
    virtual ~MainWindow();
    void importMeshs(const QStringList &_uriList);
    void loadFile(const QString& filePath);
    /// Focus the window and queue one or more OS launch paths for import.
    void openLaunchFiles(const QStringList& paths);
    void openCloudProjectFromDeepLink(const QString& ownerSlug, const QString& projectSlug);
    void importCloudDownloadedFile(const QString& localMainFile);
    void setMCPServer(MCPServer* server);

    /// Recreate Ogre render windows (e.g. after MSAA samples change in Preferences).
    void rebuildAllOgreViewports();

    /// Invokes the same merge flow as the menu action (Scene panel button).
    void triggerMergeAnimations();

    /// Invokes the same flow as Options → Material Editor (Mode Tools button).
    void triggerMaterialEditor();

    void keyPressEvent(QKeyEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    
private slots:
    void on_actionImport_triggered();
    void on_actionOpen_Scene_triggered();
    void on_actionSave_Scene_triggered();
    void on_actionMaterial_Editor_triggered();
    void on_actionAbout_triggered();
    void on_actionMerge_Animations_triggered();

    void duplicateSelected();
    void groupSelected();
    void ungroupSelected();

    void on_actionObjects_Toolbar_toggled(bool arg1);
    void on_actionTools_Toolbar_toggled(bool arg1);
    void on_actionMeshEditor_toggled(bool arg1);
    void on_actionExport_Selected_triggered();

    void chooseBgColor(void);

    void setTransformState(TransformOperator::TransformState newState);
    void createEditorViewport(void);
    void onWidgetClosing(EditorViewport* const& widget);

    void on_actionSingle_toggled(bool arg1);

    void on_action1x1_Side_by_Side_toggled(bool arg1);

    void on_action1x1_Upper_and_Lower_toggled(bool arg1);

    void on_action2x2_Grid_toggled(bool arg1);

    void on_actionAdd_Resource_location_triggered();

    void on_actionChange_Ambient_Light_triggered();

    void on_actionLight_toggled(bool arg1);

    void on_actionDark_toggled(bool arg1);

    void on_actionCustom_toggled(bool arg1);

    void on_actionVerify_Update_triggered();

    void showAIModelSettings();
    void showMCPSettings();
    void signInToQtMeshCloud();
    void signOutOfQtMeshCloud();
    void uploadFilesToQtMeshCloud();
    void showCloudProjectsDialog();
    QtMeshCloudSession* cloudSessionForToken(const QString& token);
    void showSendFeedbackDialog(const FeedbackPrefill& prefill = FeedbackPrefill{});
    bool startMCPServer(int port);
    void stopMCPServer();

public slots:
    void setPlaying(bool playing);
    void appendConsoleLine(const QString& line);

public:
    /// Accessors for the viewport display QActions so per-viewport
    /// title bars (built by `EditorViewport`) can bind their toolbuttons
    /// without forcing the whole `Ui::MainWindow` to be public. Each action
    /// is owned by `ui->menuOptions` (autogen by Qt Designer) and lives for
    /// the lifetime of MainWindow.
    QAction* actionShowGrid() const;
    QAction* actionShowNormals() const;
    QAction* actionShowMeshInfo() const;
    QAction* actionShowViewCube() const;
    Q_INVOKABLE void revealBottomTool(const QString& toolId);

private:
    Ui::MainWindow *ui;

    Ogre::Root*                 m_pRoot = nullptr;
    QList<EditorViewport*>      mDockWidgetList;

    QTimer*                     m_pTimer = nullptr;
    // TransformWidget removed — replaced by QML Inspector panel
    PrimitivesWidget*           m_pPrimitivesWidget = nullptr;

    QStringList                 mUriList;

    bool                        isPlaying = false;
    QString                     mCurrentPalette;
    QColorDialog*               customPaletteColorDialog;
    QColorDialog*               ambientLightColorDialog;

    void custom_Palette_Color_Selected(const QColor& color);
protected:
    bool frameStarted(const Ogre::FrameEvent& evt) override;
    bool frameRenderingQueued(const Ogre::FrameEvent &evt) override;
    bool frameEnded(const Ogre::FrameEvent& evt) override;

    void closeEvent(QCloseEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void initToolBar();
    void updateMergeAnimationsButton();
    const QPalette& darkPalette();

    NormalVisualizer* m_normalVisualizer = nullptr;
    MeshInfoOverlay* m_meshInfoOverlay = nullptr;
    ViewCubeController* m_viewCubeController = nullptr;
    MCPServer* m_mcpServer = nullptr;
    QQuickWidget* m_propertiesPanel = nullptr;
    QDockWidget* m_chatDock = nullptr;
    QDockWidget* m_assetBrowserDock = nullptr;
    QDockWidget* m_dopeSheetDock = nullptr;
    QDockWidget* m_curveEditorDock = nullptr;
    QDockWidget* m_bottomContextDock = nullptr;
    QDockWidget* m_consoleDock = nullptr;
    QPlainTextEdit* m_consoleEdit = nullptr;
    QToolBar* m_modeBarShell = nullptr;
    QToolBar* m_topBarStretch = nullptr;
    QQuickWidget* m_modeBar = nullptr;
    CloudAccountMenuButton* m_cloudAccountControl = nullptr;
    CloudUploadProgress* m_cloudUploadProgress = nullptr;
    QtMeshCloudSession* m_cloudSession = nullptr;
    QAction* m_cloudUploadMenuAction = nullptr;
    bool m_cloudUploadListingInFlight = false;

    void updateCloudUploadActionState();
    void startCloudPackageUpload(QtMeshCloudSession* session,
                                 const CloudPackageUploadRequest& request);

    /// View menu entries for bottom tabbed docks — checked state follows user
    /// preference, not QDockWidget::isVisible() (inactive tabs would otherwise
    /// appear unchecked).
    QAction* m_contextPanelViewAction = nullptr;
    QAction* m_consoleViewAction = nullptr;

    QMenu* m_recentFilesMenu = nullptr;
    void addToRecentFiles(const QString& filePath);
    void updateRecentFilesMenu();
    void openRecentFile();

    WelcomeScreenController* m_welcomeController = nullptr;
    QQuickWidget* m_welcomeScreen = nullptr;
    void showWelcomeScreen();
    void hideWelcomeScreen();
    void repositionWelcomeScreen();

    QLabel* m_editModeLabel = nullptr;
    /// Permanent status-bar widget for transient edit-mode hint
    /// messages (e.g. "Loop cut needs a quad mesh"). A dedicated
    /// label avoids the every-frame `showMessage()` race that
    /// `frameEnded()` causes on the status bar's main slot.
    QLabel* m_editHintLabel = nullptr;
    void updateEditModeIndicator();
    void createModeSurfaces();
    void configureBottomToolDock(QDockWidget* dock);
    void showBottomToolDock(QDockWidget* dock);
    void tabifyBottomToolDocks();
    void updateToolRailForMode();
    void setupCloudAccountStatusControl();
    void updateCloudAuthActions();
    void openCloudProjectsQmlDialog(const QString& ownerSlug = QString(),
                                    const QString& projectSlug = QString());
    void closeCloudProjectsQmlDialog();
    QObject* m_cloudProjectsWindow = nullptr;
    QQmlApplicationEngine* m_cloudProjectsEngine = nullptr;

#ifdef ENABLE_AUTO_UPDATER
    void showUpdaterDialog(bool runCheck = true);
    void showUpdateToast(const QString& version);
    QObject* m_updaterWindow = nullptr;
    QQmlApplicationEngine* m_updaterEngine = nullptr;
    QObject* m_updateToastWindow = nullptr;
    QQmlApplicationEngine* m_updateToastEngine = nullptr;
#endif
};

#endif // MAINWINDOW_H
