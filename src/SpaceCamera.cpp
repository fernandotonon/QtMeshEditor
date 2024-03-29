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
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>

#include "SpaceCamera.h"
#include "Manager.h"
#include "OgreWidget.h"
#include <QDebug>

const QPoint    SpaceCamera::invalidPoint(-1,-1);
//SpaceCamera*    SpaceCamera::mSpaceCamera = NULL;

SpaceCamera::SpaceCamera(OgreWidget* parent)
    :mSceneMgr(Manager::getSingleton()->getSceneMgr())
    ,mCameraNode(NULL)
    ,mTarget(NULL)
    ,mCamera(NULL)
    ,mCameraSpeed(0.5f)
    ,mOldPos(invalidPoint)
{
    QString name = "Camera " + QString::number(parent->getIndex());
    while (mSceneMgr->hasCamera(name.toStdString())) {
        name+=".";
    }
    mCamera = mSceneMgr->createCamera(name.toLocal8Bit().constData());

    if (mCamera == 0)
        throw std::logic_error("TPCamera::TPCamera - 'Ogre::Camera* camera' == NULL");

    mCamera->setFarClipDistance(999999999.9f);
    mCamera->setNearClipDistance( 1.0f );

    mTarget = mSceneMgr->getRootSceneNode()->createChildSceneNode();
    mTarget->setPosition(Ogre::Vector3(0.0f,1.0f,0.0f));

    mCameraNode = mTarget->createChildSceneNode();
    mCameraNode->translate(Ogre::Vector3(0.0f,0.0f,-20.0f),Ogre::Node::TS_PARENT);
    mCameraNode->lookAt(mTarget->getPosition(),Ogre::Node::TS_WORLD);
    mCameraNode->attachObject(mCamera);

    Manager::getSingleton()->getRoot()->addFrameListener(this);

    setAspectRatio(Ogre::Real(parent->width()) / Ogre::Real(parent->height()));

    setKeyMapping();
}

SpaceCamera::~SpaceCamera()
{
    if(mSceneMgr)
    {
        Manager::getSingleton()->getRoot()->removeFrameListener(this);
        mSceneMgr->destroySceneNode(mCameraNode);
        mSceneMgr->destroySceneNode(mTarget);
        mSceneMgr->destroyCamera(mCamera);
        mCameraNode = nullptr;
        mTarget     = nullptr;
        mCamera     = nullptr;
    }
}

void SpaceCamera::setKeyMapping()
{
    // Key Mapping initialisation
    // TODO push this static or at app level

    mKeyRotationMapping[Qt::Key_Up]     = Ogre::Vector2 ( 0.0,  getCameraSpeed()  );
    mKeyRotationMapping[Qt::Key_Down] 	= Ogre::Vector2 ( 0.0, -getCameraSpeed()  );
    mKeyRotationMapping[Qt::Key_Right] 	= Ogre::Vector2 ( getCameraSpeed() , 0.0  );
    mKeyRotationMapping[Qt::Key_Left]   = Ogre::Vector2 (-getCameraSpeed() , 0.0  );

    mKeyTranslationMapping[Qt::Key_W]   = Ogre::Vector2 ( 0.0,  getCameraSpeed()  );
    mKeyTranslationMapping[Qt::Key_S] 	= Ogre::Vector2 ( 0.0, -getCameraSpeed()  );
    mKeyTranslationMapping[Qt::Key_A] 	= Ogre::Vector2 ( getCameraSpeed() , 0.0  );
    mKeyTranslationMapping[Qt::Key_D]   = Ogre::Vector2 (-getCameraSpeed() , 0.0  );

    mKeyRollingMapping[Qt::Key_Q]       =  getCameraSpeed();
    mKeyRollingMapping[Qt::Key_E]       = -getCameraSpeed();
}

//////////////////////////////////////////////////////////////////////////////////
//Accessors
Ogre::Camera * SpaceCamera::getCamera()
{    return mCamera;    }

const Ogre::Quaternion& SpaceCamera::getOrientation() const
{    return mTarget->getOrientation();}

const Ogre::Real& SpaceCamera::getCameraSpeed()    const
{   return mCameraSpeed; }

//////////////////////////////////////////////////////////////////////////////////
//Mutators

void SpaceCamera::setCameraSpeed(const Ogre::Real& newSpeed)
{    mCameraSpeed = newSpeed; }

void SpaceCamera::setCameraPosition(const Ogre::Vector3 &pos)
{
    //Compute reverse polar from pos
    mTarget->lookAt(pos,Ogre::Node::TS_WORLD);
    Ogre::Vector3 zTranslation = mTarget->convertWorldToLocalPosition(pos) - mCameraNode->getPosition();
    mCameraNode->translate(Ogre::Vector3(0,0,zTranslation.z));
}

void SpaceCamera::setTargetPosition(const Ogre::Vector3 &pos)
{    mTarget->setPosition(pos); }

void SpaceCamera::setAspectRatio(const Ogre::Real& ratio)
{    mCamera->setAspectRatio(ratio); }

//////////////////////////////////////////////////////////////////////////////////
//Frame listener

void SpaceCamera::mousePressEvent(QMouseEvent *event)
{
    if ((event->button()==Qt::RightButton)
        ||(event->button()==Qt::MiddleButton))
    {
        mOldPos = event->pos();
        event->ignore();
    }

    event->ignore();
}

void SpaceCamera::mouseReleaseEvent(QMouseEvent *event)
{
    if ((event->button()==Qt::RightButton)
        ||(event->button()==Qt::MiddleButton))
    {
        mOldPos = invalidPoint;
        event->accept();
    }
    else
        event->ignore();
}

void SpaceCamera::mouseMoveEvent(QMouseEvent *event)
{
    if (mOldPos != invalidPoint)
    {
        Ogre::Real deltaX = (event->pos().x() - mOldPos.x()) * mCameraSpeed;
        Ogre::Real deltaY = (event->pos().y() - mOldPos.y()) * mCameraSpeed;

        if(event->buttons().testFlag(Qt::MiddleButton))
        {   // if Shift is pressed, translate, else Alt => roll, else rotate
            if(event->modifiers().testFlag(Qt::ShiftModifier))                
                roll(deltaX);               //Shift =>Roll (obviously locally)
            else
                arcBall(deltaX, deltaY);    //Just Middle button => arcball

            mOldPos = event->pos();
            event->accept();
        }
        else if(event->buttons().testFlag(Qt::RightButton))
        {    // Right button => panoramic
            pan( deltaX, deltaY );
            mOldPos = event->pos();
            event->accept();
        }
        else
            event->ignore();

    }
    else
        event->ignore();

}



void SpaceCamera::wheelEvent(QWheelEvent *event)
{
    int delta = event->angleDelta().y() * 10 / 60 * getCameraSpeed();
    zoom(delta);

    pan(Ogre::Vector2 ( event->angleDelta().x() * 10 / 60 * getCameraSpeed() , 0.0  ));

    event->accept();
}

bool SpaceCamera::frameEnded(const Ogre::FrameEvent& event)
{
    return true;
}

bool SpaceCamera::frameStarted(const Ogre::FrameEvent& event)
{

    if(!mRotation.isZeroLength())
        arcBall(mRotation);

    if(!mTranslation.isZeroLength())
        pan(mTranslation);

    if(mRoll != 0.0)
        roll(mRoll);

    return true;
}

void SpaceCamera::keyPressEvent(QKeyEvent *event)
{

    QMap<int, Ogre::Vector2>::iterator keyPressed = mKeyRotationMapping.find(event->key());

    if(keyPressed != mKeyRotationMapping.end())
    {
        mRotation = keyPressed.value() * mCameraSpeed;
        event->accept();
    }

    keyPressed = mKeyTranslationMapping.find(event->key());

    if(keyPressed != mKeyTranslationMapping.end())
    {
        mTranslation = keyPressed.value() * mCameraSpeed;
        event->accept();
    }

    QMap<int, Ogre::Real>::iterator keyRoll = mKeyRollingMapping.find(event->key());

    if(keyRoll != mKeyRollingMapping.end())
    {
        mRoll = keyRoll.value() * mCameraSpeed;
        event->accept();
    }

    // TODO add some customization in the UI for Camera speed
    if(event->key() == Qt::Key_Control)
    {
        setCameraSpeed(0.01f);
        event->accept();
    }
}

void SpaceCamera::keyReleaseEvent(QKeyEvent *event)
{
    QMap<int, Ogre::Vector2>::iterator keyPressed = mKeyRotationMapping.find(event->key());

    if(keyPressed != mKeyRotationMapping.end())
    {
        mRotation = Ogre::Vector2(0.0, 0.0);
        event->accept();
    }

    keyPressed = mKeyTranslationMapping.find(event->key());

    if(keyPressed != mKeyTranslationMapping.end())
    {
        mTranslation = Ogre::Vector2(0.0, 0.0);
        event->accept();
    }

    QMap<int, Ogre::Real>::iterator keyRoll = mKeyRollingMapping.find(event->key());

    if(keyRoll != mKeyRollingMapping.end())
    {
        mRoll = 0.0;
        event->accept();
    }

    if(event->key() == Qt::Key_Control)
    {
        setCameraSpeed(0.1f);
        event->accept();
    }
}

//////////////////////////////////////////////////////////////////////////////////
//Private Methods

void SpaceCamera::zoom(const int delta)
{
    Ogre::Vector3 zTranslation(0, 0, delta);
    mCameraNode->translate(zTranslation,Ogre::Node::TS_PARENT);
}

void SpaceCamera::pan(const Ogre::Real& deltaX, const Ogre::Real& deltaY)
{
    mTarget->translate(deltaX,deltaY,0,Ogre::Node::TS_LOCAL );
}

void SpaceCamera::pan(const Ogre::Vector2& translation)
{   pan(translation.x, translation.y);   }

void SpaceCamera::arcBall(const Ogre::Real& deltaX, const Ogre::Real& deltaY)
{
    Ogre::Radian rotYaw(deltaX  * 0.05f);
    Ogre::Radian rotPitch(deltaY  * 0.05f);

    mTarget->yaw( rotYaw, Ogre::Node::TS_WORLD );
    mTarget->pitch( rotPitch, Ogre::Node::TS_LOCAL );

}

void SpaceCamera::arcBall(const Ogre::Vector2 &rotation)
{    arcBall(rotation.x, rotation.y);   }

void SpaceCamera::roll(const Ogre::Real& delta)
{
    Ogre::Radian rotRoll( - delta  * 0.05f);

    mTarget->roll( rotRoll, Ogre::Node::TS_LOCAL );
}







