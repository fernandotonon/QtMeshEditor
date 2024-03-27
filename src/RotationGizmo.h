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

    const Ogre::ManualObject&     getXCircle(void) const;
    const Ogre::ManualObject&     getYCircle(void) const;
    const Ogre::ManualObject&     getZCircle(void) const;

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

