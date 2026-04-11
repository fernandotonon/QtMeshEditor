#include "GlobalDefinitions.h"

#include "ScaleGizmo.h"
#include "GizmoAxisHelpers.h"
#include "Manager.h"
#include <Ogre.h>

const float ScaleGizmo::mSolidThickness = 30.0f;
const float ScaleGizmo::mCubeSize = 3.0f; // Cube handle is 3x the shaft thickness

ScaleGizmo::ScaleGizmo(Ogre::SceneNode* linkNode, const Ogre::String &name, Ogre::Real scale)
    : m_pXaxis(nullptr), m_pYaxis(nullptr), m_pZaxis(nullptr), mFade(0.4f), mHighlighted(false)
{
    mScale = scale;

    Ogre::SceneManager* pSceneMgr = linkNode->getCreator();
    m_pXaxis = pSceneMgr->createManualObject(name + "X");
    m_pYaxis = pSceneMgr->createManualObject(name + "Y");
    m_pZaxis = pSceneMgr->createManualObject(name + "Z");

    // Default Colors
    Ogre::Real solid = 1.0f;
    mXaxisColor = Ogre::ColourValue(solid, 0, 0, solid);
    mYaxisColor = Ogre::ColourValue(0, solid, 0, solid);
    mZaxisColor = Ogre::ColourValue(0, 0, solid, solid);

    GizmoAxisHelpers::forEachAxis(m_pXaxis, m_pYaxis, m_pZaxis,
                                  [](Ogre::ManualObject* axis) {
                                      axis->setRenderQueueGroup(ZORDER_OVERLAY);
                                  });

    setQueryFlags(0);

    GizmoAxisHelpers::forEachAxis(m_pXaxis, m_pYaxis, m_pZaxis,
                                  [linkNode](Ogre::ManualObject* axis) {
                                      linkNode->attachObject(axis);
                                  });
}

ScaleGizmo::~ScaleGizmo()
{
    Ogre::SceneManager* pSceneMgr = m_pXaxis->getParentSceneNode()->getCreator();
    GizmoAxisHelpers::forEachAxis(m_pXaxis, m_pYaxis, m_pZaxis,
                                  [pSceneMgr](Ogre::ManualObject* axis) {
                                      pSceneMgr->destroyManualObject(axis);
                                  });
}

void ScaleGizmo::createCube(Ogre::ManualObject* obj, const Ogre::ColourValue& colour,
                            const Ogre::Vector3& center, float halfSize)
{
    // 8 vertices of a cube centered at 'center'
    float cx = center.x, cy = center.y, cz = center.z;
    float s = halfSize;

    int base = obj->getCurrentVertexCount();

    obj->position(Ogre::Vector3(cx - s, cy + s, cz + s));
    obj->position(Ogre::Vector3(cx - s, cy - s, cz + s));
    obj->position(Ogre::Vector3(cx + s, cy - s, cz + s));
    obj->position(Ogre::Vector3(cx + s, cy + s, cz + s));

    obj->position(Ogre::Vector3(cx - s, cy + s, cz - s));
    obj->position(Ogre::Vector3(cx - s, cy - s, cz - s));
    obj->position(Ogre::Vector3(cx + s, cy - s, cz - s));
    obj->position(Ogre::Vector3(cx + s, cy + s, cz - s));

    // 6 faces (12 triangles)
    // Front
    obj->quad(base + 0, base + 1, base + 2, base + 3);
    // Back
    obj->quad(base + 7, base + 6, base + 5, base + 4);
    // Top
    obj->quad(base + 0, base + 3, base + 7, base + 4);
    // Bottom
    obj->quad(base + 2, base + 1, base + 5, base + 6);
    // Right
    obj->quad(base + 3, base + 2, base + 6, base + 7);
    // Left
    obj->quad(base + 1, base + 0, base + 4, base + 5);
}

void ScaleGizmo::createXaxis(const Ogre::ColourValue& colour)
{
    createSolidXaxis(colour);
}

void ScaleGizmo::createYaxis(const Ogre::ColourValue& colour)
{
    createSolidYaxis(colour);
}

void ScaleGizmo::createZaxis(const Ogre::ColourValue& colour)
{
    createSolidZaxis(colour);
}

void ScaleGizmo::createSolidXaxis(const Ogre::ColourValue& colour)
{
    float thickness = mScale / mSolidThickness;
    float shaftLength = mScale * 0.85f;
    float cubeHalf = thickness * mCubeSize;

    m_pXaxis->clear();
    m_pXaxis->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_TRIANGLE_LIST);
        m_pXaxis->colour(colour);

        // Shaft - thin box from origin to shaftLength along +X
        m_pXaxis->position(Ogre::Vector3(0,            thickness,  thickness));
        m_pXaxis->position(Ogre::Vector3(0,           -thickness,  thickness));
        m_pXaxis->position(Ogre::Vector3(shaftLength, -thickness,  thickness));
        m_pXaxis->position(Ogre::Vector3(shaftLength,  thickness,  thickness));

        m_pXaxis->position(Ogre::Vector3(0,            thickness, -thickness));
        m_pXaxis->position(Ogre::Vector3(0,           -thickness, -thickness));
        m_pXaxis->position(Ogre::Vector3(shaftLength, -thickness, -thickness));
        m_pXaxis->position(Ogre::Vector3(shaftLength,  thickness, -thickness));

        m_pXaxis->quad(0, 1, 2, 3);
        m_pXaxis->quad(7, 6, 5, 4);
        m_pXaxis->quad(0, 3, 7, 4);
        m_pXaxis->quad(2, 1, 5, 6);
        m_pXaxis->quad(3, 2, 6, 7);
        m_pXaxis->quad(1, 0, 4, 5);

        // Cube handle at end of shaft
        createCube(m_pXaxis, colour, Ogre::Vector3(mScale, 0, 0), cubeHalf);

    m_pXaxis->end();
}

void ScaleGizmo::createSolidYaxis(const Ogre::ColourValue& colour)
{
    float thickness = mScale / mSolidThickness;
    float shaftLength = mScale * 0.85f;
    float cubeHalf = thickness * mCubeSize;

    m_pYaxis->clear();
    m_pYaxis->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_TRIANGLE_LIST);
        m_pYaxis->colour(colour);

        // Shaft along +Y
        m_pYaxis->position(Ogre::Vector3( thickness, 0,            thickness));
        m_pYaxis->position(Ogre::Vector3( thickness, shaftLength,  thickness));
        m_pYaxis->position(Ogre::Vector3(-thickness, shaftLength,  thickness));
        m_pYaxis->position(Ogre::Vector3(-thickness, 0,            thickness));

        m_pYaxis->position(Ogre::Vector3( thickness, 0,           -thickness));
        m_pYaxis->position(Ogre::Vector3( thickness, shaftLength, -thickness));
        m_pYaxis->position(Ogre::Vector3(-thickness, shaftLength, -thickness));
        m_pYaxis->position(Ogre::Vector3(-thickness, 0,           -thickness));

        m_pYaxis->quad(0, 1, 2, 3);
        m_pYaxis->quad(7, 6, 5, 4);
        m_pYaxis->quad(1, 0, 4, 5);
        m_pYaxis->quad(3, 2, 6, 7);
        m_pYaxis->quad(2, 1, 5, 6);
        m_pYaxis->quad(0, 3, 7, 4);

        // Cube handle at end
        createCube(m_pYaxis, colour, Ogre::Vector3(0, mScale, 0), cubeHalf);

    m_pYaxis->end();
}

void ScaleGizmo::createSolidZaxis(const Ogre::ColourValue& colour)
{
    float thickness = mScale / mSolidThickness;
    float shaftLength = mScale * 0.85f;
    float cubeHalf = thickness * mCubeSize;

    m_pZaxis->clear();
    m_pZaxis->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_TRIANGLE_LIST);
        m_pZaxis->colour(colour);

        // Shaft along +Z
        m_pZaxis->position(Ogre::Vector3( thickness,  thickness, 0));
        m_pZaxis->position(Ogre::Vector3( thickness,  thickness, shaftLength));
        m_pZaxis->position(Ogre::Vector3( thickness, -thickness, shaftLength));
        m_pZaxis->position(Ogre::Vector3( thickness, -thickness, 0));

        m_pZaxis->position(Ogre::Vector3(-thickness,  thickness, 0));
        m_pZaxis->position(Ogre::Vector3(-thickness,  thickness, shaftLength));
        m_pZaxis->position(Ogre::Vector3(-thickness, -thickness, shaftLength));
        m_pZaxis->position(Ogre::Vector3(-thickness, -thickness, 0));

        m_pZaxis->quad(0, 1, 2, 3);
        m_pZaxis->quad(7, 6, 5, 4);
        m_pZaxis->quad(1, 0, 4, 5);
        m_pZaxis->quad(3, 2, 6, 7);
        m_pZaxis->quad(2, 1, 5, 6);
        m_pZaxis->quad(0, 3, 7, 4);

        // Cube handle at end
        createCube(m_pZaxis, colour, Ogre::Vector3(0, 0, mScale), cubeHalf);

    m_pZaxis->end();
}

//////////////////////////////////////////
// Accessors

bool ScaleGizmo::isHighlighted(void) const
{   return mHighlighted; }

Ogre::uint32 ScaleGizmo::getQueryFlags(void) const
{   return m_pXaxis->getQueryFlags(); }

const Ogre::Real& ScaleGizmo::getFading(void) const
{   return mFade; }

const Ogre::ColourValue& ScaleGizmo::getXaxisColour(void) const
{   return mXaxisColor; }

const Ogre::ColourValue& ScaleGizmo::getYaxisColour(void) const
{   return mYaxisColor; }

const Ogre::ColourValue& ScaleGizmo::getZaxisColour(void) const
{   return mZaxisColor; }

const Ogre::Real& ScaleGizmo::getScale(void) const
{   return mScale; }

const Ogre::ManualObject& ScaleGizmo::getXAxis() const
{   return *m_pXaxis; }

const Ogre::ManualObject& ScaleGizmo::getYAxis() const
{   return *m_pYaxis; }

const Ogre::ManualObject& ScaleGizmo::getZAxis() const
{   return *m_pZaxis; }

//////////////////////////////////////////
// Mutators

void ScaleGizmo::setVisible(bool visible)
{
    if(visible)
        createAxis();
    GizmoAxisHelpers::forEachAxis(m_pXaxis, m_pYaxis, m_pZaxis,
                                  [visible](Ogre::ManualObject* axis) {
                                      axis->setVisible(visible);
                                  });
}

void ScaleGizmo::setQueryFlags(Ogre::uint32 flags)
{
    GizmoAxisHelpers::forEachAxis(m_pXaxis, m_pYaxis, m_pZaxis,
                                  [flags](Ogre::ManualObject* axis) {
                                      axis->setQueryFlags(flags);
                                  });
}

void ScaleGizmo::setFading(const Ogre::Real& fade)
{   mFade = fade; }

void ScaleGizmo::setXaxisColour(const Ogre::ColourValue &colour)
{
    mXaxisColor = colour;
    createXaxis(mXaxisColor);
}

void ScaleGizmo::setYaxisColour(const Ogre::ColourValue &colour)
{
    mYaxisColor = colour;
    createYaxis(mYaxisColor);
}

void ScaleGizmo::setZaxisColour(const Ogre::ColourValue &colour)
{
    mZaxisColor = colour;
    createZaxis(mZaxisColor);
}

void ScaleGizmo::setScale(const Ogre::Real& scale)
{
    mScale = scale;
    createAxis();
}

void ScaleGizmo::createAxis(void)
{
    createXaxis(mXaxisColor);
    createYaxis(mYaxisColor);
    createZaxis(mZaxisColor);

    // Update bounding boxes to encompass cube handles
    float cubeHalf = (mScale / mSolidThickness) * mCubeSize;

    GizmoAxisHelpers::forEachAxisIndexed(m_pXaxis, m_pYaxis, m_pZaxis,
                                         [this, cubeHalf](GizmoAxisHelpers::Axis axis, Ogre::ManualObject* axisObject) {
                                             axisObject->setBoundingBox(
                                                 GizmoAxisHelpers::makeAxisBoundingBox(
                                                     axis, 0.0f, mScale + cubeHalf, cubeHalf));
                                         });

    mHighlighted = false;
}

Ogre::Vector3 ScaleGizmo::highlightAxis(const Ogre::MovableObject* obj)
{
    const GizmoAxisHelpers::Axis highlightedAxis =
        GizmoAxisHelpers::axisFromObject(obj, m_pXaxis, m_pYaxis, m_pZaxis);

    GizmoAxisHelpers::dispatchAxis(
        highlightedAxis,
        [this]() {
            createSolidXaxis(mXaxisColor);
            createYaxis(mYaxisColor * mFade);
            createZaxis(mZaxisColor * mFade);
        },
        [this]() {
            createSolidYaxis(mYaxisColor);
            createXaxis(mXaxisColor * mFade);
            createZaxis(mZaxisColor * mFade);
        },
        [this]() {
            createSolidZaxis(mZaxisColor);
            createXaxis(mXaxisColor * mFade);
            createYaxis(mYaxisColor * mFade);
        },
        [this]() { createAxis(); });

    if (highlightedAxis == GizmoAxisHelpers::Axis::None) {
        return Ogre::Vector3::ZERO;
    }

    mHighlighted = true;
    return GizmoAxisHelpers::axisToUnitVector(highlightedAxis);
}
