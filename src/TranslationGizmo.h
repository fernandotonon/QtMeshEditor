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

    const Ogre::ManualObject&     getXAxis(void) const;
    const Ogre::ManualObject&     getYAxis(void) const;
    const Ogre::ManualObject&     getZAxis(void) const;

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

