/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) HogPog Team (www.hogpog.com.br)

The MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------------
*/

#ifndef __OGREWIDGET_H__
#define __OGREWIDGET_H__

#include <Ogre.h>
#include <QtWidgets>
#include <OgreFrameListener.h>

class SpaceCamera;
namespace Ogre
{
    class Root;
    class RenderWindow;
    class Viewport;
    class SceneManager;
}

class OgreWidget : public QWidget, public Ogre::FrameListener
{
  Q_OBJECT

 public:
  explicit OgreWidget( QWidget* parent = nullptr );
  ~OgreWidget() override;

  int getIndex()    const;
  QColor getBackgroundColor() const;
  const Ogre::Viewport* getViewport() const;
  void setBackgroundColor(const QColor& c);

protected:
  /*virtual*/ void initOgreWindow(void);

  QPaintEngine* paintEngine() const override;
  void resizeEvent(QResizeEvent *e) override;
  void moveEvent(QMoveEvent *e) override;

  // Keyboard & Mouse event
  void mousePressEvent(QMouseEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void wheelEvent(QWheelEvent *e) override;

  void focusInEvent(QFocusEvent* e) override;
  void focusOutEvent(QFocusEvent* e) override;

  // Ogre listener
  bool frameStarted(const Ogre::FrameEvent& evt) override;
  bool frameRenderingQueued(const Ogre::FrameEvent &evt) override;
  bool frameEnded(const Ogre::FrameEvent& evt) override;

public: //keyboard entries are driven by the dockwidget because of focus issue
  void keyPressEvent(QKeyEvent *e) override;
  void keyReleaseEvent(QKeyEvent *e) override;

signals:
    void focusOnWidget(OgreWidget* ogreWidget);

private:
  Ogre::Root*           mOgreRoot = nullptr;
  Ogre::RenderWindow*   mOgreWindow = nullptr;
  Ogre::Viewport*       mViewport = nullptr;
  Ogre::SceneManager*   mSceneMgr = nullptr;
  std::unique_ptr<SpaceCamera> mCamera;
};

#endif //__OGREWIDGET_H__
