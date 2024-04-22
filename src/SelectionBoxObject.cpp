#include "SelectionBoxObject.h"

#include "GlobalDefinitions.h"
#include "Manager.h"

#include <QtDebug>

SelectionBoxObject::SelectionBoxObject(const Ogre::String& name)
    : Ogre::ManualObject(name)
{
    setRenderQueueGroup(ZORDER_OVERLAY); // when using this, ensure Depth Check is Off in the material
    setUseIdentityProjection(true);
    setUseIdentityView(true);
    setQueryFlags(0);
}

const Ogre::ColourValue& SelectionBoxObject::getBoxColour() const
{   return mBoxColour;  }

void SelectionBoxObject::setBoxColour(const Ogre::ColourValue &colour)
{   mBoxColour = colour; }

void SelectionBoxObject::drawBox(float left, float top, float right, float bottom)
{
    //upper left corner (-1,1) - bottom right corner (1,-1)
    clear();
    begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_LINE_STRIP);
    colour(mBoxColour);
            position(left, top, -1);
            position(right, top, -1);
            position(right, bottom, -1);
            position(left, bottom, -1);
            position(left, top, -1);
    end();

    setBoundingBox(Ogre::AxisAlignedBox::BOX_INFINITE);
}

void SelectionBoxObject::drawBox(const Ogre::Vector2& topLeft, const Ogre::Vector2& bottomRight)
{
    drawBox(topLeft.x, topLeft.y, bottomRight.x, bottomRight.y);
}
