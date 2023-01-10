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

#ifndef SPACECAMERA_H
#define SPACECAMERA_H

#include <QPoint>
#include <QMap>

#include <OgreFrameListener.h>
#include <OgreCamera.h>


class OgreWidget;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

class SpaceCamera : Ogre::FrameListener
{
    public:
        SpaceCamera(OgreWidget* parent);
        ~SpaceCamera();

        // Accessors
        Ogre::Camera*           getCamera();
        const Ogre::Quaternion& getOrientation() const;
        const Ogre::Real&       getCameraSpeed() const;

        // Mutators
        void setCameraSpeed(const Ogre::Real& newSpeed);
        void setCameraPosition(const Ogre::Vector3 &pos);
        void setTargetPosition(const Ogre::Vector3 &pos);
        void setAspectRatio(const Ogre::Real& ratio);

        /// FrameListener
        bool frameStarted(const Ogre::FrameEvent& event);
        bool frameEnded(const Ogre::FrameEvent& event);

        virtual void keyPressEvent(QKeyEvent *event);
        virtual void keyReleaseEvent(QKeyEvent *event);

        virtual void mousePressEvent(QMouseEvent *event);
        virtual void mouseReleaseEvent(QMouseEvent *event);
        virtual void mouseMoveEvent(QMouseEvent *event);
        virtual void wheelEvent(QWheelEvent *event);

    private:
        Ogre::SceneNode*        mCameraNode; // Node da camera
        Ogre::SceneNode*        mTarget;
        Ogre::Camera*           mCamera; // Ogre camera
        Ogre::Real              mCameraSpeed;
        Ogre::SceneManager*     mSceneMgr; //SceneManager

        static const QPoint     invalidPoint;
        QPoint                  mOldPos;
        Ogre::Vector2           mRotation;
        Ogre::Vector2           mTranslation;
        Ogre::Real              mRoll;

        QMap<int, Ogre::Vector2> mKeyRotationMapping;
        QMap<int, Ogre::Vector2> mKeyTranslationMapping;
        QMap<int, Ogre::Real>    mKeyRollingMapping;

    private:
        void setKeyMapping();
        void zoom(const int delta);
        void pan(const Ogre::Real& deltaX, const Ogre::Real& deltaY);
        void pan(const Ogre::Vector2& translation);
        void arcBall(const Ogre::Vector2& rotation);
        void arcBall(const Ogre::Real& deltaX, const Ogre::Real& deltaY);
        void roll(const Ogre::Real& delta);

};

#endif // SPACECAMERA_H
