#include "GlobalDefinitions.h"

#include "TranslationGizmo.h"
#include "Manager.h"
#include <Ogre.h>

const float TranslationGizmo::mSolidThickness = 80.0f;


// TODO add a square to allow plane trnaslation with mouse


TranslationGizmo::TranslationGizmo(Ogre::SceneNode* linkNode, const Ogre::String &name, Ogre::Real scale, bool leftHandCs)
    : m_pXaxis(0), m_pYaxis(0), m_pZaxis(0), mFade(0.4f), mHighlighted(false)
{
    mScale        = scale;
    mLeftHandCs   = leftHandCs;

    Ogre::SceneManager* pSceneMgr = linkNode->getCreator();
    //Creating the manual objects
    m_pXaxis = pSceneMgr->createManualObject(name + "X");
    m_pYaxis = pSceneMgr->createManualObject(name + "Y");
    m_pZaxis = pSceneMgr->createManualObject(name + "Z");


    //Default Color
    Ogre::Real solid=1.0f;
    mXaxisColor = Ogre::ColourValue(solid, 0, 0, solid);
    mYaxisColor = Ogre::ColourValue(0, solid, 0, solid);
    mZaxisColor = Ogre::ColourValue(0, 0, solid, solid);

    m_pXaxis->setRenderQueueGroup(ZORDER_OVERLAY); // when using this, ensure Depth Check is Off in the material
    m_pYaxis->setRenderQueueGroup(ZORDER_OVERLAY);
    m_pZaxis->setRenderQueueGroup(ZORDER_OVERLAY);

    setQueryFlags(0);

    linkNode->attachObject(m_pXaxis);
    linkNode->attachObject(m_pYaxis);
    linkNode->attachObject(m_pZaxis);
}

TranslationGizmo::~TranslationGizmo()
{
    Ogre::SceneManager* pSceneMgr = m_pXaxis->getParentSceneNode()->getCreator();
    //TODO unload material ??

    pSceneMgr->destroyManualObject(m_pXaxis);
    pSceneMgr->destroyManualObject(m_pYaxis);
    pSceneMgr->destroyManualObject(m_pZaxis);

}

void TranslationGizmo::createXaxis(const Ogre::ColourValue& colour)
{
    m_pXaxis->clear();
    m_pXaxis->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_LINE_LIST);
        m_pXaxis->colour(colour);
        m_pXaxis->position(Ogre::Vector3(0, 0, 0));
        m_pXaxis->position(Ogre::Vector3(mScale, 0, 0));
    m_pXaxis->end();
}

void TranslationGizmo::createYaxis(const Ogre::ColourValue& colour)
{
    m_pYaxis->clear();
    m_pYaxis->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_LINE_LIST);
        m_pYaxis->colour(colour);
        m_pYaxis->position(Ogre::Vector3(0, 0, 0));
        m_pYaxis->position(Ogre::Vector3(0, mScale, 0));
    m_pYaxis->end();
}

void TranslationGizmo::createZaxis(const Ogre::ColourValue& colour)
{
    Ogre::Real z = 1.0f;

    if(mLeftHandCs)
       z = -1.0f;

    m_pZaxis->clear();
    m_pZaxis->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_LINE_LIST);
        m_pZaxis->colour(colour);
        m_pZaxis->position(Ogre::Vector3(0, 0, 0));
        m_pZaxis->position(Ogre::Vector3(0, 0, z*mScale));
    m_pZaxis->end();
}

void TranslationGizmo::createSolidXaxis(const Ogre::ColourValue& colour)
{
    float thickness = mScale / mSolidThickness;

    m_pXaxis->clear();
    m_pXaxis->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_TRIANGLE_LIST);
        m_pXaxis->colour(colour);

        m_pXaxis->position(Ogre::Vector3(0,       thickness,  thickness));
        m_pXaxis->position(Ogre::Vector3(0,      -thickness,  thickness));
        m_pXaxis->position(Ogre::Vector3(mScale, -thickness,  thickness));
        m_pXaxis->position(Ogre::Vector3(mScale,  thickness,  thickness));

        m_pXaxis->position(Ogre::Vector3(0,       thickness, -thickness));
        m_pXaxis->position(Ogre::Vector3(0,      -thickness, -thickness));
        m_pXaxis->position(Ogre::Vector3(mScale, -thickness, -thickness));
        m_pXaxis->position(Ogre::Vector3(mScale,  thickness, -thickness));

        m_pXaxis->quad(0, 1, 2, 3);
        m_pXaxis->quad(7, 6, 5, 4);
        m_pXaxis->quad(0, 3, 7, 4);
        m_pXaxis->quad(2, 1, 5, 6);
        m_pXaxis->quad(3, 2, 6, 7);
        m_pXaxis->quad(1, 0, 4, 5);

    m_pXaxis->end();
}

void TranslationGizmo::createSolidYaxis(const Ogre::ColourValue& colour)
{
    float thickness = mScale / mSolidThickness;

    m_pYaxis->clear();
    m_pYaxis->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_TRIANGLE_LIST);
        m_pYaxis->colour(colour);

        m_pYaxis->position(Ogre::Vector3( thickness, 0,       thickness));
        m_pYaxis->position(Ogre::Vector3( thickness, mScale,  thickness));
        m_pYaxis->position(Ogre::Vector3(-thickness, mScale,  thickness));
        m_pYaxis->position(Ogre::Vector3(-thickness, 0,       thickness));

        m_pYaxis->position(Ogre::Vector3( thickness, 0,      -thickness));
        m_pYaxis->position(Ogre::Vector3( thickness, mScale, -thickness));
        m_pYaxis->position(Ogre::Vector3(-thickness, mScale, -thickness));
        m_pYaxis->position(Ogre::Vector3(-thickness, 0,      -thickness));

        m_pYaxis->quad(0, 1, 2, 3);
        m_pYaxis->quad(7, 6, 5, 4);
        m_pYaxis->quad(1, 0, 4, 5);
        m_pYaxis->quad(3, 2, 6, 7);
        m_pYaxis->quad(2, 1, 5, 6);
        m_pYaxis->quad(0, 3, 7, 4);

    m_pYaxis->end();
}

void TranslationGizmo::createSolidZaxis(const Ogre::ColourValue& colour)
{
    float thickness = mScale / mSolidThickness;
    Ogre::Real z = 1.0f;

    if(mLeftHandCs)
       z = -1.0f;

    m_pZaxis->clear();
    m_pZaxis->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_TRIANGLE_LIST);
        m_pZaxis->colour(colour);

        m_pZaxis->position(Ogre::Vector3( thickness,  thickness,        0));
        m_pZaxis->position(Ogre::Vector3( thickness,  thickness, z*mScale));
        m_pZaxis->position(Ogre::Vector3( thickness, -thickness, z*mScale));
        m_pZaxis->position(Ogre::Vector3( thickness, -thickness,        0));

        m_pZaxis->position(Ogre::Vector3(-thickness,  thickness,        0));
        m_pZaxis->position(Ogre::Vector3(-thickness,  thickness, z*mScale));
        m_pZaxis->position(Ogre::Vector3(-thickness, -thickness, z*mScale));
        m_pZaxis->position(Ogre::Vector3(-thickness, -thickness,        0));

    if(mLeftHandCs)
    {
        m_pZaxis->quad(3, 2, 1, 0);
        m_pZaxis->quad(4, 5, 6, 7);
        m_pZaxis->quad(0, 1, 5, 4);
        m_pZaxis->quad(2, 3, 7, 6);
        m_pZaxis->quad(1, 2, 6, 5);
        m_pZaxis->quad(0, 4, 7, 3);
    }
     else
    {
        m_pZaxis->quad(0, 1, 2, 3);
        m_pZaxis->quad(7, 6, 5, 4);
        m_pZaxis->quad(1, 0, 4, 5);
        m_pZaxis->quad(3, 2, 6, 7);
        m_pZaxis->quad(2, 1, 5, 6);
        m_pZaxis->quad(0, 3, 7, 4);
    }

    m_pZaxis->end();
}

//////////////////////////////////////////
// Accessors

bool TranslationGizmo::isHighlighted(void) const
{   return (mHighlighted);    }

Ogre::uint32 TranslationGizmo::getQueryFlags(void)   const
{ return (m_pXaxis->getQueryFlags());  }

const Ogre::Real& TranslationGizmo::getFading(void)   const
{   return (mFade); }
bool TranslationGizmo::isLeftHandCS(void) const
{   return (mLeftHandCs);   }

const Ogre::ColourValue& TranslationGizmo::getXaxisColour (void) const
{   return (mXaxisColor);   }

const Ogre::ColourValue& TranslationGizmo::getYaxisColour (void) const
{   return (mYaxisColor);   }

const Ogre::ColourValue& TranslationGizmo::getZaxisColour (void) const
{   return (mZaxisColor);   }

const Ogre::Real& TranslationGizmo::getScale(void)    const
{   return (mScale); }
const Ogre::ManualObject &TranslationGizmo::getXAxis() const
{ return *m_pXaxis; }

const Ogre::ManualObject &TranslationGizmo::getYAxis() const
{ return *m_pYaxis; }

const Ogre::ManualObject &TranslationGizmo::getZAxis() const
{ return *m_pZaxis; }
//////////////////////////////////////////
// Mutators

void TranslationGizmo::setLeftHandCS (bool leftHandcs)
{
    if(mLeftHandCs == leftHandcs)
        return;

    mLeftHandCs = leftHandcs;
    createZaxis(mZaxisColor);
}

void TranslationGizmo::setVisible(bool visible)
{
    if(visible)
        createAxis();
    m_pXaxis->setVisible(visible);
    m_pYaxis->setVisible(visible);
    m_pZaxis->setVisible(visible);
}

void TranslationGizmo::setQueryFlags(Ogre::uint32 flags)
{
    m_pXaxis->setQueryFlags(flags);
    m_pYaxis->setQueryFlags(flags);
    m_pZaxis->setQueryFlags(flags);

}

void TranslationGizmo::setFading(const Ogre::Real& fade)
{    mFade = fade;  }

void TranslationGizmo::setXaxisColour ( const Ogre::ColourValue &colour)
{
    mXaxisColor = colour;
    createXaxis(mXaxisColor);
}

void TranslationGizmo::setYaxisColour ( const Ogre::ColourValue &colour)
{
    mYaxisColor = colour;
    createYaxis(mYaxisColor);
}

void TranslationGizmo::setZaxisColour ( const Ogre::ColourValue &colour)
{
    mZaxisColor = colour;
    createZaxis(mZaxisColor);
}

void TranslationGizmo::setScale ( const Ogre::Real& scale)
{
    mScale = scale;
    createAxis();
}

void TranslationGizmo::createAxis(void)
{
    createXaxis(mXaxisColor);
    createYaxis(mYaxisColor);
    createZaxis(mZaxisColor);

    // Increasing size of Boundinx box (actually 0 thickness.....)
    float bbSize = mScale/mSolidThickness;

    Ogre::AxisAlignedBox boundingBox = m_pXaxis->getBoundingBox();
    boundingBox.setExtents(Ogre::Vector3(0,-bbSize,-bbSize),Ogre::Vector3(mScale,bbSize,bbSize));
    m_pXaxis->setBoundingBox(boundingBox);

    boundingBox = m_pYaxis->getBoundingBox();
    boundingBox.setExtents(Ogre::Vector3(-bbSize,0,-bbSize),Ogre::Vector3(bbSize,mScale,bbSize));
    m_pYaxis->setBoundingBox(boundingBox);

    boundingBox = m_pZaxis->getBoundingBox();
    boundingBox.setExtents(Ogre::Vector3(-bbSize,-bbSize,0),Ogre::Vector3(bbSize,bbSize,mScale));
    m_pZaxis->setBoundingBox(boundingBox);

    mHighlighted = false;
}

Ogre::Vector3 TranslationGizmo::highlightAxis(const Ogre::MovableObject* obj)
{
    Ogre::Vector3 result = Ogre::Vector3::ZERO;

    if(obj == m_pXaxis)
    {
        createSolidXaxis(mXaxisColor);
        createYaxis(mYaxisColor*mFade);
        createZaxis(mZaxisColor*mFade);
        mHighlighted = true;
        result =  Ogre::Vector3::UNIT_X;
    }
    else if(obj == m_pYaxis)
    {
        createSolidYaxis(mYaxisColor);
        createXaxis(mXaxisColor*mFade);
        createZaxis(mZaxisColor*mFade);
        result =  Ogre::Vector3::UNIT_Y;
        mHighlighted = true;
    }
    else if(obj == m_pZaxis)
    {
        createSolidZaxis(mZaxisColor);
        createXaxis(mXaxisColor*mFade);
        createYaxis(mYaxisColor*mFade);
        result =  Ogre::Vector3::UNIT_Z;
        mHighlighted = true;
    }
    else
    {
        createAxis();

    }

    return result;
}


