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

#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>

#include "SpaceCamera.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "OgreWidget.h"
#include <QDebug>

const QPoint    SpaceCamera::invalidPoint(-1,-1);
//SpaceCamera*    SpaceCamera::mSpaceCamera = NULL;

SpaceCamera::SpaceCamera(OgreWidget* parent)
    :mSceneMgr(Manager::getSingleton()->getSceneMgr())
    ,mCameraNode(nullptr)
    ,mTarget(nullptr)
    ,mCamera(nullptr)
    ,mCameraSpeed(0.5f)
    ,mOldPos(invalidPoint)
{
    QString name = "Camera " + QString::number(parent->getIndex());
    while (mSceneMgr->hasCamera(name.toStdString())) {
        name+=".";
    }
    mCamera = mSceneMgr->createCamera(name.toLocal8Bit().constData());

    if (mCamera == nullptr)
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

    mKeyRotationMapping[Qt::Key_Up]     = Ogre::Vector2 ( 0.0,  getCameraSpeed() * 0.1f );
    mKeyRotationMapping[Qt::Key_Down] 	= Ogre::Vector2 ( 0.0, -getCameraSpeed() * 0.1f );
    mKeyRotationMapping[Qt::Key_Right] 	= Ogre::Vector2 ( getCameraSpeed() * 0.1f, 0.0  );
    mKeyRotationMapping[Qt::Key_Left]   = Ogre::Vector2 (-getCameraSpeed() * 0.1f, 0.0  );

    // Note: WASD/QE removed to avoid conflict with Unity-style transform shortcuts
    // (Q=Select, W=Translate, E=Rotate, R=Scale).
    // Camera movement via mouse (middle=orbit, right=pan, scroll=zoom) and arrow keys.
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
{
    mCameraSpeed = newSpeed;
    mBaseCameraSpeed = newSpeed;
}

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

void SpaceCamera::animateToOrientation(const Ogre::Quaternion& target, float duration)
{
    if (duration <= 0.0f) {
        // Instant snap
        mTarget->setOrientation(target);
        mAnimating = false;
        return;
    }
    mAnimStartOrientation = mTarget->getOrientation();
    mAnimTargetOrientation = target;
    mAnimElapsed = 0.0f;
    mAnimDuration = duration;
    mAnimating = true;
}

void SpaceCamera::mousePressEvent(QMouseEvent *event)
{
    // Cancel animation on mouse press for immediate user control
    mAnimating = false;

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
    Ogre::Real xDelta = event->angleDelta().x() / 120.0f;
    Ogre::Real yDelta = event->angleDelta().y() / 120.0f;

    if (event->modifiers().testFlag(Qt::ControlModifier))
    {
        // Ctrl + scroll = zoom (for mouse wheel users)
        zoom(yDelta);
    }
    else if (event->source() == Qt::MouseEventSynthesizedBySystem)
    {
        // Trackpad two-finger swipe = pan in both axes
        Ogre::Real panScale = 3.0f;
        pan(Ogre::Vector2(xDelta * panScale, yDelta * panScale));
    }
    else
    {
        // Mouse scroll wheel = zoom
        zoom(yDelta);
        if (std::abs(xDelta) > 0.01f)
            pan(Ogre::Vector2(xDelta * 3.0f, 0.0f));
    }

    event->accept();
}

bool SpaceCamera::frameEnded(const Ogre::FrameEvent& event)
{
    return true;
}

bool SpaceCamera::frameStarted(const Ogre::FrameEvent& event)
{
    // Smooth orientation animation
    if (mAnimating) {
        mAnimElapsed += event.timeSinceLastFrame;
        float t = mAnimElapsed / mAnimDuration;
        if (t >= 1.0f) {
            t = 1.0f;
            mAnimating = false;
        }
        // Smoothstep interpolation
        t = t * t * (3.0f - 2.0f * t);
        Ogre::Quaternion q = Ogre::Quaternion::Slerp(t, mAnimStartOrientation, mAnimTargetOrientation, true);
        mTarget->setOrientation(q);
        return true; // Skip keyboard-driven motion during animation
    }

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
        mCameraSpeed = mBaseCameraSpeed * 0.1f; // Ctrl = 10x slower (don't update base)
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
        setCameraSpeed(mBaseCameraSpeed); // Restore base speed
        event->accept();
    }
}

//////////////////////////////////////////////////////////////////////////////////
//Private Methods

void SpaceCamera::zoomByDelta(Ogre::Real delta)
{
    zoom(delta);
}

void SpaceCamera::zoom(Ogre::Real delta)
{
    // Proportional zoom: step size scales with distance to target,
    // so zooming feels consistent regardless of model size.
    Ogre::Real distance = std::abs(mCameraNode->getPosition().z);
    Ogre::Real minDistance = 0.01f;
    Ogre::Real zoomFactor = 0.15f; // 15% of distance per scroll tick

    Ogre::Real step = delta * std::max(distance, minDistance) * zoomFactor;

    Ogre::Vector3 newPos = mCameraNode->getPosition();
    newPos.z += step;

    // Don't pass through the target
    if (newPos.z > -minDistance)
        newPos.z = -minDistance;

    mCameraNode->setPosition(newPos);
}

void SpaceCamera::pan(const Ogre::Real& deltaX, const Ogre::Real& deltaY)
{
    // Proportional pan: step scales with distance to target
    Ogre::Real distance = std::abs(mCameraNode->getPosition().z);
    Ogre::Real scale = std::max(distance, 0.01f) * 0.01f;
    mTarget->translate(deltaX * scale, deltaY * scale, 0, Ogre::Node::TS_LOCAL);
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

void SpaceCamera::frameSelection()
{
    SelectionSet* sel = SelectionSet::getSingleton();
    if (!sel || sel->isEmpty())
        return;

    // Compute aggregate AABB from selection
    Ogre::AxisAlignedBox aabb;
    aabb.setNull();

    if (sel->hasNodes())
    {
        for (Ogre::SceneNode* node : sel->getNodesSelectionList())
        {
            // Get world AABB of all attached objects
            auto it = node->getAttachedObjectIterator();
            while (it.hasMoreElements())
            {
                Ogre::MovableObject* obj = it.getNext();
                aabb.merge(obj->getWorldBoundingBox(true));
            }
            // If node has no attached objects, at least include its position
            if (!node->numAttachedObjects())
                aabb.merge(node->_getDerivedPosition());
        }
    }
    else if (sel->hasEntities())
    {
        for (Ogre::Entity* ent : sel->getEntitiesSelectionList())
            aabb.merge(ent->getWorldBoundingBox(true));
    }

    if (aabb.isNull() || aabb.isInfinite())
        return;

    Ogre::Vector3 center = aabb.getCenter();
    Ogre::Real radius = (aabb.getMaximum() - aabb.getMinimum()).length() * 0.5f;

    // Ensure minimum radius for point-like selections
    if (radius < 0.1f)
        radius = 1.0f;

    // Move camera target to selection center
    mTarget->setPosition(center);

    // Compute distance to fit bounding sphere in view
    Ogre::Radian fovY = mCamera->getFOVy();
    Ogre::Real aspectRatio = mCamera->getAspectRatio();
    Ogre::Radian fovX = Ogre::Radian(2.0f * std::atan(std::tan(fovY.valueRadians() * 0.5f) * aspectRatio));
    Ogre::Radian fov = std::min(fovX, fovY);
    Ogre::Real distance = radius / std::sin(fov.valueRadians() * 0.5f);

    // Add a bit of padding
    distance *= 1.2f;

    mCameraNode->setPosition(0, 0, -distance);
}







