#ifndef SELECTION_BOX_OBJECT_H
#define SELECTION_BOX_OBJECT_H

#include <OgrePrerequisites.h>
#include <Ogre.h>
#include <OgreBlendMode.h>
#include <OgreManualObject.h>

class SelectionBoxObject : public Ogre::ManualObject
{
public :
    explicit SelectionBoxObject(const Ogre::String& name);
    ~SelectionBoxObject() override = default ;

    const Ogre::ColourValue& getBoxColour() const;

    void setBoxColour(const Ogre::ColourValue& colour);

    void drawBox(float left, float top, float right, float bottom);
    void drawBox(const Ogre::Vector2& topLeft, const Ogre::Vector2& bottomRight);

private :
    Ogre::ColourValue mBoxColour = Ogre::ColourValue(0.8f,0.8f,0.8f,0.8f);
};

#endif //SELECTION_BOX_OBJECT_H


