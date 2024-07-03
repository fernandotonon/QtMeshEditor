#include <QtDebug>

#include "GlobalDefinitions.h"

#include "RotationGizmo.h"
#include "Manager.h"

#include <Ogre.h>

////////////////////////////////////////////////////////////////////////////////
// Static Initialisation
const float RotationGizmo::mAccuracy = 30.0f;
const float RotationGizmo::mSolidThickness = 40.0f;

////////////////////////////////////////////////////////////////////////////////
// Constructor & destructor

RotationGizmo::RotationGizmo(Ogre::SceneNode* linkNode, const Ogre::String &name, Ogre::Real scale)
    : m_pXCircle(nullptr), m_pYCircle(nullptr), m_pZCircle(nullptr),mFade(0.4f), mHighlighted(false), mScale(scale)
{
    Ogre::SceneManager* pSceneMgr = linkNode->getCreator();
    //Creating the manual objects
    m_pXCircle = pSceneMgr->createManualObject(name + "X");
    m_pYCircle = pSceneMgr->createManualObject(name + "Y");
    m_pZCircle = pSceneMgr->createManualObject(name + "Z");

    //Default Color
    Ogre::Real solid=1.0f;
    mXaxisColor = Ogre::ColourValue(solid, 0, 0, solid);
    mYaxisColor = Ogre::ColourValue(0, solid, 0, solid);
    mZaxisColor = Ogre::ColourValue(0, 0, solid, solid);

    m_pXCircle->setRenderQueueGroup(ZORDER_OVERLAY); // when using this, ensure Depth Check is Off in the material
    m_pYCircle->setRenderQueueGroup(ZORDER_OVERLAY);
    m_pZCircle->setRenderQueueGroup(ZORDER_OVERLAY);

    setQueryFlags(0);

    linkNode->attachObject(m_pXCircle);
    linkNode->attachObject(m_pYCircle);
    linkNode->attachObject(m_pZCircle);
}

RotationGizmo::~RotationGizmo()
{
    Ogre::SceneManager* pSceneMgr = m_pXCircle->getParentSceneNode()->getCreator();
    //TODO unload material ??

    pSceneMgr->destroyManualObject(m_pXCircle);
    pSceneMgr->destroyManualObject(m_pYCircle);
    pSceneMgr->destroyManualObject(m_pZCircle);
}

void RotationGizmo::createXCircle(const Ogre::ColourValue& colour)
{
    m_pXCircle->clear();
    m_pXCircle->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_LINE_STRIP);
    m_pXCircle->colour(colour);
    for(int deg = 0; deg <= 360; deg += 1)
    {
        float theta = Ogre::Degree(deg).valueRadians();
        m_pXCircle->position(0, cos(theta)*mScale, sin(theta)*mScale);
    }
    m_pXCircle->end();
}

void RotationGizmo::createYCircle(const Ogre::ColourValue& colour)
{
    m_pYCircle->clear();
    m_pYCircle->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_LINE_STRIP);
    m_pYCircle->colour(colour);
    for(int deg = 0; deg <= 360; deg += 1)
    {
        float theta = Ogre::Degree(deg).valueRadians();
        m_pYCircle->position(cos(theta)*mScale, 0, sin(theta)*mScale);
    }
    m_pYCircle->end();
}

void RotationGizmo::createZCircle(const Ogre::ColourValue& colour)
{
    m_pZCircle->clear();
    m_pZCircle->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_LINE_STRIP);
    m_pZCircle->colour(colour);
    for(int deg = 0; deg <= 360; deg += 1)
    {
        float theta = Ogre::Degree(static_cast<float>(deg)).valueRadians();
        m_pZCircle->position(cos(theta)*mScale, sin(theta)*mScale,0);
    }
    m_pZCircle->end();
}

void RotationGizmo::createSolidXCircle(const Ogre::ColourValue& colour)
{
    float thickness = mScale / mSolidThickness;
    unsigned point_index = 0;

    m_pXCircle->clear();
    m_pXCircle->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_TRIANGLE_LIST );
    m_pXCircle->colour(colour);
    for(int deg = 0; deg <= 360; deg += 1)
    {
        float theta = Ogre::Degree(static_cast<float>(deg)).valueRadians();
        m_pXCircle->position(0, mScale * cos(theta), mScale * sin(theta));
        m_pXCircle->position(0, mScale * cos(theta - Ogre::Math::PI / mAccuracy), mScale * sin(theta - Ogre::Math::PI / mAccuracy));
        m_pXCircle->position(0, (mScale - thickness) * cos(theta - Ogre::Math::PI / mAccuracy), (mScale - thickness) * sin(theta - Ogre::Math::PI / mAccuracy));
        m_pXCircle->position(0, (mScale - thickness) * cos(theta), (mScale - thickness) * sin(theta));
        // Join the 4 vertices created above to form a quad.
        m_pXCircle->quad(point_index, point_index + 1, point_index + 2, point_index + 3);
        m_pXCircle->quad(point_index + 3, point_index + 2, point_index + 1, point_index);
        point_index += 4;
    }
    m_pXCircle->end();
}

void RotationGizmo::createSolidYCircle(const Ogre::ColourValue& colour)
{
    float thickness = mScale / mSolidThickness;
    unsigned point_index = 0;

    m_pYCircle->clear();
    m_pYCircle->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_TRIANGLE_LIST );
    m_pYCircle->colour(colour);
    for(int deg = 0; deg <= 360; deg += 1)
    {
        float theta = Ogre::Degree(static_cast<float>(deg)).valueRadians();
        m_pYCircle->position(mScale * cos(theta), 0, mScale * sin(theta));
        m_pYCircle->position(mScale * cos(theta - Ogre::Math::PI / mAccuracy), 0, mScale * sin(theta - Ogre::Math::PI / mAccuracy));
        m_pYCircle->position((mScale - thickness) * cos(theta - Ogre::Math::PI / mAccuracy), 0,(mScale - thickness) * sin(theta - Ogre::Math::PI / mAccuracy));
        m_pYCircle->position((mScale - thickness) * cos(theta), 0, (mScale - thickness) * sin(theta));
        // Join the 4 vertices created above to form a quad.
        m_pYCircle->quad(point_index, point_index + 1, point_index + 2, point_index + 3);
        m_pYCircle->quad(point_index + 3, point_index + 2, point_index + 1, point_index);
        point_index += 4;
    }
    m_pYCircle->end();
}

void RotationGizmo::createSolidZCircle(const Ogre::ColourValue& colour)
{
    float thickness = mScale / mSolidThickness;
    unsigned point_index = 0;

    m_pZCircle->clear();
    m_pZCircle->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_TRIANGLE_LIST );
    m_pZCircle->colour(colour);
    for(int deg = 0; deg <= 360; deg += 1)
    {
        float theta = Ogre::Degree(static_cast<float>(deg)).valueRadians();
        m_pZCircle->position(mScale * cos(theta), mScale * sin(theta), 0);
        m_pZCircle->position(mScale * cos(theta - Ogre::Math::PI / mAccuracy), mScale * sin(theta - Ogre::Math::PI / mAccuracy), 0);
        m_pZCircle->position((mScale - thickness) * cos(theta - Ogre::Math::PI / mAccuracy), (mScale - thickness) * sin(theta - Ogre::Math::PI / mAccuracy), 0);
        m_pZCircle->position((mScale - thickness) * cos(theta), (mScale - thickness) * sin(theta), 0);
        // Join the 4 vertices created above to form a quad.
        m_pZCircle->quad(point_index, point_index + 1, point_index + 2, point_index + 3);
        m_pZCircle->quad(point_index + 3, point_index + 2, point_index + 1, point_index);
        point_index += 4;
    }
    m_pZCircle->end();
}

//////////////////////////////////////////
// Accessors

bool RotationGizmo::isHighlighted(void) const
{   return mHighlighted;    }

Ogre::uint32 RotationGizmo::getQueryFlags(void)   const
{ return m_pXCircle->getQueryFlags();  }

const Ogre::Real& RotationGizmo::getFading(void)   const
{   return mFade; }

const Ogre::ColourValue& RotationGizmo::getXaxisColour (void) const
{   return mXaxisColor;   }

const Ogre::ColourValue& RotationGizmo::getYaxisColour (void) const
{   return mYaxisColor;   }

const Ogre::ColourValue& RotationGizmo::getZaxisColour (void) const
{   return mZaxisColor;   }

const Ogre::Real& RotationGizmo::getScale(void) const
{   return mScale;  }

const Ogre::ManualObject &RotationGizmo::getXCircle() const
{   return *m_pXCircle; }

const Ogre::ManualObject &RotationGizmo::getYCircle() const
{    return *m_pYCircle; }

const Ogre::ManualObject &RotationGizmo::getZCircle() const
{   return *m_pZCircle; }

//////////////////////////////////////////
// Mutators

void RotationGizmo::setVisible(bool visible)
{
    if(visible)
        createCircles();
    m_pXCircle->setVisible(visible);
    m_pYCircle->setVisible(visible);
    m_pZCircle->setVisible(visible);
}

void RotationGizmo::setQueryFlags(Ogre::uint32 flags)
{
    m_pXCircle->setQueryFlags(flags);
    m_pYCircle->setQueryFlags(flags);
    m_pZCircle->setQueryFlags(flags);

}

void RotationGizmo::setFading(const Ogre::Real& fade)
{    mFade = fade;  }

void RotationGizmo::setXaxisColour ( const Ogre::ColourValue &colour)
{
    mXaxisColor = colour;
    createXCircle(mXaxisColor);
}

void RotationGizmo::setYaxisColour ( const Ogre::ColourValue &colour)
{
    mYaxisColor = colour;
    createYCircle(mYaxisColor);
}

void RotationGizmo::setZaxisColour ( const Ogre::ColourValue &colour)
{
    mZaxisColor = colour;
    createZCircle(mZaxisColor);
}

void RotationGizmo::setScale ( const Ogre::Real& scale )
{
    mScale = scale;
    createCircles();
}

void RotationGizmo::createCircles(void)
{
    createXCircle(mXaxisColor);
    createYCircle(mYaxisColor);
    createZCircle(mZaxisColor);
    mHighlighted = false;
}

Ogre::Vector3 RotationGizmo::highlightCircle(const Ogre::MovableObject* obj)
{
    Ogre::Vector3 result = Ogre::Vector3::ZERO;

    if(obj == m_pXCircle)
    {
        createSolidXCircle(mXaxisColor);
        createYCircle(mYaxisColor*mFade);
        createZCircle(mZaxisColor*mFade);
        result =  Ogre::Vector3::UNIT_X;
        mHighlighted = true;
    }
    else if(obj == m_pYCircle)
    {
        createSolidYCircle(mYaxisColor);
        createXCircle(mXaxisColor*mFade);
        createZCircle(mZaxisColor*mFade);
        result =  Ogre::Vector3::UNIT_Y;
        mHighlighted = true;
    }
    else if(obj == m_pZCircle)
    {
        createSolidZCircle(mZaxisColor);
        createXCircle(mXaxisColor*mFade);
        createYCircle(mYaxisColor*mFade);
        result =  Ogre::Vector3::UNIT_Z;
        mHighlighted = true;
    }
    else
    {
        createCircles();
    }

    return result;
}
