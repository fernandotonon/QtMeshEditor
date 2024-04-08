#include <QDebug>
#include <QTimer>
#include <QCoreApplication>
#include <QTimer>

#include <Ogre.h>

#include "GlobalDefinitions.h"

#include "OgreWidget.h"
#include "Manager.h"
#include "SpaceCamera.h"
#include "EditorViewport.h"
#include "QtInputManager.h"

OgreWidget::OgreWidget( QWidget *parent ):
    QWidget( parent )
{
    setAttribute( Qt::WA_PaintOnScreen );
    setAttribute( Qt::WA_OpaquePaintEvent );
    setMinimumSize(100,100);

    setFocusPolicy(Qt::ClickFocus); //one click to get focus

    initOgreWindow();
}

OgreWidget::~OgreWidget()
{

    if(mOgreWindow)
    {
        mOgreWindow->removeAllViewports();
        mViewport = nullptr;
    }
    if(mCamera)
    {
        delete mCamera;
        mCamera = nullptr;
    }
    if(mOgreRoot)
    {
        mOgreRoot->removeFrameListener(this);
        mOgreRoot->detachRenderTarget(mOgreWindow);
        mOgreWindow->setActive(false);
        mOgreWindow = nullptr;
    }

    destroy();
}


int OgreWidget::getIndex() const
{
    return (qobject_cast<EditorViewport *>(parent()))->getIndex();
}

const QColor OgreWidget::getBackgroundColor() const
{
    if(mViewport)
    {
        Ogre::ColourValue bgColour = mViewport->getBackgroundColour();
        return QColor::fromRgbF(bgColour.r, bgColour.g, bgColour.b);
    }
    else
        return QColor(Qt::black);
}

const Ogre::Viewport* OgreWidget::getViewport() const
{   return mViewport;   }

void OgreWidget::setBackgroundColor(const QColor& c)
{
    Ogre::ColourValue ogreColour;
    ogreColour.setAsARGB(c.rgba());

    if(mViewport)
        mViewport->setBackgroundColour(ogreColour);
}


void OgreWidget::initOgreWindow(void)
{
    mOgreRoot = Manager::getSingleton()->getRoot();

    // Get the parameters of the window QT created
  Ogre::String winHandle;
#ifdef WIN32
  // Windows code
  winHandle = Ogre::StringConverter::toString((unsigned long)(this->parentWidget()->winId()));
#else
        winHandle  = Ogre::StringConverter::toString(winId());
#endif

  Ogre::NameValuePairList params;

  params["externalWindowHandle"] = winHandle;
#ifdef Q_OS_MACOS
  // code for Mac
  params["macAPI"] = "cocoa";
  params["macAPICocoaUseNSView"] = "true";
#endif

  QString name = "Viewport " + QString::number(getIndex());
  while (mOgreRoot->getRenderTarget(name.toStdString())) {
        name+=".";
  }

  mOgreWindow = mOgreRoot->createRenderWindow( name.toStdString().data(),
                           width(),
                           height(),
                           false,
                           &params );

  mOgreWindow->setActive(true);

  mCamera =  new SpaceCamera(this);

  mViewport = mOgreWindow->addViewport( mCamera->getCamera() );
  mViewport->setBackgroundColour( Ogre::ColourValue( 0,0,0 ) );
  mViewport->setVisibilityMask(SCENE_VISIBILITY_FLAGS);

  mOgreRoot->addFrameListener(this);
}

void OgreWidget::paintEvent(QPaintEvent *e)
{
    return QWidget::paintEvent(e);
}

bool OgreWidget::frameStarted(const Ogre::FrameEvent& e)
{
    return true;
}

bool OgreWidget::frameRenderingQueued(const Ogre::FrameEvent &e)
{
    return true;
}

bool OgreWidget::frameEnded(const Ogre::FrameEvent& e)
{
    if(mOgreWindow)
    {
        mOgreWindow->windowMovedOrResized(); //TODO: Improve it someday, it is there to avoid creatin window in whrong aspect ratio
        mOgreWindow->update();
    }

    return true;
}

QPaintEngine* OgreWidget:: paintEngine() const
{    return 0;  }

void OgreWidget::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);

    if(e->isAccepted())
    {
        const QSize &newSize = e->size();
        if(mOgreWindow)
        {
            mOgreWindow->resize(newSize.width(), newSize.height());
            mOgreWindow->windowMovedOrResized();
        }
        if(mCamera)
            mCamera->setAspectRatio((Ogre::Real)width()/(Ogre::Real)height());
        update();
    }
}

void OgreWidget::moveEvent(QMoveEvent *e)
{
    QWidget::moveEvent(e);

    if(e->isAccepted() && mOgreWindow)
    {
        mOgreWindow->windowMovedOrResized();
        update();
    }
}

////////////////////////////////////////////////////////////////////////////////////////
// Mouse & Keyboard event Widget level

void OgreWidget::keyPressEvent(QKeyEvent *e)
{
    mCamera->keyPressEvent(e);

    //  KEYBOARD ENTRY FOR DEBUG PURPOSE ONLY
    if(e->key () == Qt::Key_Escape)
    {
        //emit closeApplication();
    }           ////////////// Debug /////////////////////
    else if(e->key () == Qt::Key_F1)
    {
        qDebug()<<"F1 has been pressed";

    }
    else if(e->key () == Qt::Key_F2)
    {

    }
    else if(e->key () == Qt::Key_F3)
    {

    }
    else if(e->key () == Qt::Key_F4)
    {

    }
    else if(e->key () == Qt::Key_F5)
    {


    }       ////////////// Debug /////////////////////
    else
    {
        e->ignore();
    }
}

void OgreWidget::keyReleaseEvent(QKeyEvent *e)
{   mCamera->keyReleaseEvent(e);  }

void OgreWidget::wheelEvent(QWheelEvent *e)
{
    QtInputManager::getInstance().wheelEvent(e);
    mCamera->wheelEvent(e);
}

void OgreWidget::mousePressEvent(QMouseEvent *e)
{
    QtInputManager::getInstance().mousePressEvent(e);

    mCamera->mousePressEvent(e);

    if(e->buttons().testFlag(Qt::MiddleButton))
    {
        QCursor cursor = this->cursor();
        cursor.setShape(Qt::SizeAllCursor);
        QWidget::setCursor(cursor);

        e->accept();

    }
    else if(e->buttons().testFlag(Qt::LeftButton))
    {

        QCursor cursor = this->cursor();
        cursor.setShape(Qt::ClosedHandCursor);
        QWidget::setCursor(cursor);


        e->accept();
    }
    else
    {
        e->ignore();
    }
}

void OgreWidget::mouseMoveEvent(QMouseEvent *e)
{
    QtInputManager::getInstance().mouseMoveEvent(e);

    mCamera->mouseMoveEvent(e);
}

void OgreWidget::mouseReleaseEvent(QMouseEvent *e)
{
    QtInputManager::getInstance().mouseReleaseEvent(e);
    mCamera->mouseReleaseEvent(e);

    QCursor cursor = this->cursor();
    cursor.setShape(Qt::ArrowCursor);
    QWidget::setCursor(cursor);

}

void OgreWidget::focusInEvent(QFocusEvent* e)
{

    emit focusOnWidget(this);
    parentWidget()->update(); //required to show the highlight on the DockWidget
    mViewport->setVisibilityMask(SCENE_VISIBILITY_FLAGS | GUI_VISIBILITY_FLAGS);
    e->accept();

}

void OgreWidget::focusOutEvent(QFocusEvent* e)
{

    parentWidget()->update(); //required to show the highlight on the DockWidget
    mViewport->setVisibilityMask(SCENE_VISIBILITY_FLAGS);
    e->accept();
}

