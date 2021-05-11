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

#ifndef ROTATION_GIZMO_H
#define ROTATION_GIZMO_H

#include <QObject>
#include <OgrePrerequisites.h>
#include <OgreBlendMode.h>

namespace Ogre{
    class SceneNode;
    class ManualObject;
    class ColourValue;
}
class RotationGizmo
{
public:
    RotationGizmo(Ogre::SceneNode* linkNode, const Ogre::String &name = "RotationGizmo", Ogre::Real scale = 1.0);
    ~RotationGizmo();

private:
    Ogre::Real              mScale;
    Ogre::Real              mFade;
    static const float      mAccuracy;
    static const float      mSolidThickness;

    bool                    mHighlighted;

    Ogre::ManualObject*     m_pXCircle;
    Ogre::ManualObject*     m_pYCircle;
    Ogre::ManualObject*     m_pZCircle;

    Ogre::ColourValue       mXaxisColor;
    Ogre::ColourValue       mYaxisColor;
    Ogre::ColourValue       mZaxisColor;


protected :
    virtual void createXCircle(const Ogre::ColourValue& colour);
    virtual void createYCircle(const Ogre::ColourValue& colour);
    virtual void createZCircle(const Ogre::ColourValue& colour);

    virtual void createSolidXCircle(const Ogre::ColourValue& colour);
    virtual void createSolidYCircle(const Ogre::ColourValue& colour);
    virtual void createSolidZCircle(const Ogre::ColourValue& colour);

public:
    // Accessors
    bool isHighlighted(void) const;

    Ogre::uint32 getQueryFlags(void)   const;
    const Ogre::Real& getFading(void)   const;

    const Ogre::ColourValue& getXaxisColour (void) const;
    const Ogre::ColourValue& getYaxisColour (void) const;
    const Ogre::ColourValue& getZaxisColour (void) const;

    const Ogre::Real& getScale(void)    const;

    // Mutators
    void setVisible(bool visible = true);
    void setQueryFlags(Ogre::uint32 flags = 0xFF);
    void setFading(const Ogre::Real& fade);

    void setXaxisColour ( const Ogre::ColourValue &colour);
    void setYaxisColour ( const Ogre::ColourValue &colour);
    void setZaxisColour ( const Ogre::ColourValue &colour);

    void setScale ( const Ogre::Real& scale );

    virtual void createCircles(void);
    Ogre::Vector3 highlightCircle(const Ogre::MovableObject* obj);

};


#endif //ROTATION_GIZMO_H

