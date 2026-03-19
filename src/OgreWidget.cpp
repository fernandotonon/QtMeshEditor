/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

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

#include <QDebug>
#include <QTimer>
#include <QCoreApplication>
#include <QTimer>
#include <QNativeGestureEvent>

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
    // Safely clean up OGRE resources
    // Order is important: remove listeners first, then destroy camera, then detach render target, then remove viewports
    
    if(mOgreRoot)
    {
        try {
            // Remove frame listener first
            mOgreRoot->removeFrameListener(this);
        } catch (...) {
            // Ignore exceptions during shutdown
        }
    }
    
    // Destroy camera BEFORE removing viewports (viewport may reference camera)
    // This must happen before removing viewports
    mCamera.reset();
    
    if(mOgreWindow)
    {
        try {
            // Remove all viewports (they should be cleared after camera destruction)
            try {
                mOgreWindow->removeAllViewports();
            } catch (...) {
                // Ignore exceptions during shutdown
            }
            
            // Detach render target from root if root still exists
            if(mOgreRoot)
            {
                try {
                    mOgreRoot->detachRenderTarget(mOgreWindow);
                } catch (...) {
                    // Ignore exceptions during shutdown
                }
            }
            
            // Deactivate window
            try {
                mOgreWindow->setActive(false);
            } catch (...) {
                // Ignore exceptions during shutdown
            }
            
            mViewport = nullptr;
            mOgreWindow = nullptr;
        } catch (...) {
            // If something goes wrong, just nullify pointers
            mViewport = nullptr;
            mOgreWindow = nullptr;
        }
    }

    destroy();
}


int OgreWidget::getIndex() const
{
    return (qobject_cast<EditorViewport *>(parent()))->getIndex();
}

QColor OgreWidget::getBackgroundColor() const
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

    mCamera = std::make_unique<SpaceCamera>(this);

    mViewport = mOgreWindow->addViewport( mCamera->getCamera() );
    mViewport->setBackgroundColour( Ogre::ColourValue( 0.118f, 0.118f, 0.118f ) );
    mViewport->setVisibilityMask(SCENE_VISIBILITY_FLAGS);
    mViewport->setMaterialScheme(Ogre::MSN_SHADERGEN);

    mOgreRoot->addFrameListener(this);
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
    // Safety check: don't access window if widget is being destroyed
    if(mOgreWindow && mOgreRoot)
    {
        try {
            mOgreWindow->windowMovedOrResized();
            mOgreWindow->update();
        } catch (...) {
            // Ignore exceptions during shutdown
            return false;
        }
    }

    return true;
}

QPaintEngine* OgreWidget:: paintEngine() const
{    return nullptr;  }

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
    e->ignore();
}

void OgreWidget::keyReleaseEvent(QKeyEvent *e)
{   mCamera->keyReleaseEvent(e);  }

void OgreWidget::wheelEvent(QWheelEvent *e)
{
    QtInputManager::getInstance().wheelEvent(e);
    mCamera->wheelEvent(e);
}

bool OgreWidget::event(QEvent *e)
{
    if (e->type() == QEvent::NativeGesture) {
        auto *gestureEvent = static_cast<QNativeGestureEvent*>(e);
        if (gestureEvent->gestureType() == Qt::ZoomNativeGesture) {
            // value() is typically ±0.01..0.1 per gesture event; scale to usable range
            Ogre::Real delta = static_cast<Ogre::Real>(gestureEvent->value() * 5.0);
            mCamera->zoomByDelta(delta);
            e->accept();
            return true;
        }
    }
    return QWidget::event(e);
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

