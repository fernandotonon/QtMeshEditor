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

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QComboBox>
#include <QTableWidget>
#include <OgreFrameListener.h>

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
    explicit MainWindow(QWidget *parent = 0);
    ~MainWindow();
    void importMeshs(const QStringList &_uriList);
    
private slots:
    void on_actionImport_triggered();
    void on_actionMaterial_Editor_triggered();
    void on_actionAbout_triggered();

    void on_actionObjects_Toolbar_toggled(bool arg1);
    void on_actionTools_Toolbar_toggled(bool arg1);
    void on_actionMeshEditor_toggled(bool arg1);
    void on_actionExport_Selected_triggered();

    void chooseBgColor(void);

    void setTransformState(int newState);
    void createEditorViewport(void);
    void onWidgetClosing(EditorViewport* const& widget);
    void ogreUpdate(void);

    void on_actionSingle_toggled(bool arg1);

    void on_action1x1_Side_by_Side_toggled(bool arg1);

    void on_action1x1_Upper_and_Lower_toggled(bool arg1);

    void on_actionAdd_Resource_location_triggered();

public slots:
    void setPlaying(bool playing);

private:
    Ui::MainWindow *ui;

    Ogre::Root*                 m_pRoot;
    QList<EditorViewport*>      mDockWidgetList;

    QTimer*                     m_pTimer;
    TransformWidget*            m_pTransformWidget;
    PrimitivesWidget*           m_pPrimitivesWidget;
    MaterialWidget*             m_pMaterialWidget;

    QStringList                 mUriList;

    bool                        isPlaying;

protected:
    bool frameStarted(const Ogre::FrameEvent& evt);
    bool frameRenderingQueued(const Ogre::FrameEvent &evt);
    bool frameEnded(const Ogre::FrameEvent& evt);

    void keyPressEvent(QKeyEvent *event);
    void keyReleaseEvent(QKeyEvent *event);
    void dropEvent(QDropEvent *event);
    void dragEnterEvent(QDragEnterEvent *event);

private:
    void initToolBar();

};

#endif // MAINWINDOW_H
