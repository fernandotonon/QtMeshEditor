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

#include "SelectionBoxObject.h"

#include "GlobalDefinitions.h"
#include "Manager.h"

#include <Ogre.h>
#include <QtDebug>

SelectionBoxObject::SelectionBoxObject(const Ogre::String& name)
    : Ogre::ManualObject(name), mBoxColour(Ogre::ColourValue(0.8f,0.8f,0.8f,0.8f))
{
    setRenderQueueGroup(ZORDER_OVERLAY); // when using this, ensure Depth Check is Off in the material
    setUseIdentityProjection(true);
    setUseIdentityView(true);
    setQueryFlags(0);
}

SelectionBoxObject::~SelectionBoxObject()
{
    //TODO Unload ressource material ???
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
