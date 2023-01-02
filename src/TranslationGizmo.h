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

#ifndef TRANSLATION_GIZMO_H
#define TRANSLATION_GIZMO_H

#include <OgrePrerequisites.h>
#include <OgreBlendMode.h>
namespace Ogre
{
    class SceneNode;
    class ManualObject;
    class MovableObject;
    class ColourValue;
}


class TranslationGizmo
{

public:
    TranslationGizmo( Ogre::SceneNode* linkNode, const Ogre::String &name = "Axis", Ogre::Real scale = 1.0f, bool leftHandCs = false);
    ~TranslationGizmo();

    // Accessors
    bool isLeftHandCS(void) const;
    bool isHighlighted(void) const;

    Ogre::uint32 getQueryFlags(void)   const;
    const Ogre::Real& getFading (void) const;

    const Ogre::ColourValue& getXaxisColour (void) const;
    const Ogre::ColourValue& getYaxisColour (void) const;
    const Ogre::ColourValue& getZaxisColour (void) const;

    const Ogre::Real& getScale(void)    const;

    // Mutators
    void setVisible(bool visible = true);
    void setQueryFlags(Ogre::uint32 flags = 0xFF);
    void setLeftHandCS (bool leftHandcs = true);
    void setFading (const Ogre::Real& fade);

    void setXaxisColour ( const Ogre::ColourValue &colour);
    void setYaxisColour ( const Ogre::ColourValue &colour);
    void setZaxisColour ( const Ogre::ColourValue &colour);

    void setScale ( const Ogre::Real& scale );

    virtual void createAxis(void);
    Ogre::Vector3 highlightAxis(const Ogre::MovableObject* obj);

protected :
    virtual void createXaxis(const Ogre::ColourValue& colour);
    virtual void createYaxis(const Ogre::ColourValue& colour);
    virtual void createZaxis(const Ogre::ColourValue& colour);

    virtual void createSolidXaxis(const Ogre::ColourValue& colour);
    virtual void createSolidYaxis(const Ogre::ColourValue& colour);
    virtual void createSolidZaxis(const Ogre::ColourValue& colour);

private:
    Ogre::Real              mScale;
    Ogre::Real              mFade;  //Color fading
    bool                    mLeftHandCs;
    static const float      mSolidThickness;

    bool                    mHighlighted;

    Ogre::ManualObject*     m_pXaxis;
    Ogre::ManualObject*     m_pYaxis;
    Ogre::ManualObject*     m_pZaxis;

    Ogre::ColourValue       mXaxisColor;
    Ogre::ColourValue       mYaxisColor;
    Ogre::ColourValue       mZaxisColor;

};

#endif //TRANSLATION_GIZMO_H

