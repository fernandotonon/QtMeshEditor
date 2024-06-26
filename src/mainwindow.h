#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QComboBox>
#include <QTableWidget>
#include <QColorDialog>
#include <OgreFrameListener.h>
#include <QNetworkAccessManager>

#include "TransformOperator.h"

namespace Ui {
class MainWindow;
}
class EditorViewport;
class TransformWidget;
class PrimitivesWidget;
class MaterialWidget;

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
    explicit MainWindow(QWidget *parent = nullptr);
    virtual ~MainWindow();
    void importMeshs(const QStringList &_uriList);

    void keyPressEvent(QKeyEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    
private slots:
    void on_actionImport_triggered();
    void on_actionMaterial_Editor_triggered();
    void on_actionAbout_triggered();

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

    void on_actionAdd_Resource_location_triggered();

    void on_actionChange_Ambient_Light_triggered();

    void on_actionLight_toggled(bool arg1);

    void on_actionDark_toggled(bool arg1);

    void on_actionCustom_toggled(bool arg1);

    void on_actionVerify_Update_triggered();

public slots:
    void setPlaying(bool playing);

private:
    Ui::MainWindow *ui;

    Ogre::Root*                 m_pRoot = nullptr;
    QList<EditorViewport*>      mDockWidgetList;

    QTimer*                     m_pTimer = nullptr;
    TransformWidget*            m_pTransformWidget = nullptr;
    PrimitivesWidget*           m_pPrimitivesWidget = nullptr;
    MaterialWidget*             m_pMaterialWidget = nullptr;

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

    void keyReleaseEvent(QKeyEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;

private:
    void initToolBar();
    const QPalette& darkPalette();
};

#endif // MAINWINDOW_H
