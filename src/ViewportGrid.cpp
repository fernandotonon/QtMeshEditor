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

#include <QtDebug>

#include "GlobalDefinitions.h"

#include "ViewportGrid.h"
#include "Manager.h"

#include <Ogre.h>


////////////////////////////////////////////////////////////////////////////////
// Constructor & destructor

ViewportGrid::ViewportGrid(const Ogre::ColourValue& colour, const unsigned int scale)
    : m_pGridLine(nullptr), m_pGridLineNode(nullptr), mGridColor(colour),mScale(scale), mFade(0.3f)
{
    if(Manager::getSingleton()->getSceneMgr()->hasManualObject("GridLine"))
        m_pGridLine = Manager::getSingleton()->getSceneMgr()->getManualObject("GridLine");
    else
        m_pGridLine = Manager::getSingleton()->getSceneMgr()->createManualObject("GridLine");

    if(Manager::getSingleton()->hasSceneNode("GridLine_node")){
        m_pGridLineNode=Manager::getSingleton()->getSceneNode("GridLine_node");
    }
    else {
        m_pGridLineNode=Manager::getSingleton()->addSceneNode("GridLine_node");
        m_pGridLineNode->attachObject(m_pGridLine);
    }

    m_pGridLine->setRenderQueueGroup(Ogre::RENDER_QUEUE_BACKGROUND/*ZORDER_OVERLAY*/); // when using this, ensure Depth Check is Off in the material

    mPosition = Ogre::Vector3::ZERO;

    setQueryFlags(0);

    createGrid();

}

ViewportGrid::~ViewportGrid()
{
    Ogre::SceneManager* pSceneMgr = m_pGridLine->getParentSceneNode()->getCreator();

    pSceneMgr->destroyManualObject(m_pGridLine);
    pSceneMgr->destroySceneNode(m_pGridLineNode);
}


void ViewportGrid::createGrid()
{
    m_pGridLine->clear();
    m_pGridLine->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_LINE_LIST);

    for(int x = mPosition.x-mScale;x<mPosition.x+mScale;++x)
    {
        for(int z = mPosition.z-mScale;z<mPosition.z+mScale;++z)
        {
            m_pGridLine->colour(mGridColor);
            m_pGridLine->position(x,mPosition.y,z);
            m_pGridLine->position(x,mPosition.y,z+1);
            m_pGridLine->position(x,mPosition.y,z+1);
            m_pGridLine->position(x+1,mPosition.y,z+1);
            m_pGridLine->position(x+1,mPosition.y,z+1);
            m_pGridLine->position(x+1,mPosition.y,z);
            m_pGridLine->position(x+1,mPosition.y,z);
            m_pGridLine->position(x,mPosition.y,z);

            m_pGridLine->colour(mGridColor * mFade);
            m_pGridLine->position(x+0.5,mPosition.y,z);
            m_pGridLine->position(x+0.5,mPosition.y,z+1);
            m_pGridLine->position(x,mPosition.y,z+0.5);
            m_pGridLine->position(x+1,mPosition.y,z+0.5);

        }
    }
    m_pGridLine->end();
}

//////////////////////////////////////////
// Accessors

Ogre::uint32 ViewportGrid::getQueryFlags(void)   const
{ return (m_pGridLine->getQueryFlags());  }

const Ogre::Real& ViewportGrid::getFading(void)   const
{   return (mFade); }

const Ogre::ColourValue& ViewportGrid::getColour (void) const
{   return (mGridColor);   }

const unsigned int ViewportGrid::getScale(void) const
{   return (mScale);  }

Ogre::Vector3 ViewportGrid::getPosition() const
{    return mPosition; }

//////////////////////////////////////////
// Mutators

void ViewportGrid::setVisible(bool visible)
{
    m_pGridLineNode->setVisible(visible);
}

void ViewportGrid::setQueryFlags(Ogre::uint32 flags)
{
    m_pGridLine->setQueryFlags(flags);
}

void ViewportGrid::setPosition(const Ogre::Vector3 &position)
{
    mPosition = position;
    m_pGridLineNode->setPosition(position);
}
