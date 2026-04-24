#include "GlobalDefinitions.h"

#include "TranslationGizmo.h"
#include "GizmoAxisHelpers.h"
#include "Manager.h"
#include <Ogre.h>

const float TranslationGizmo::mSolidThickness = 30.0f; 


TranslationGizmo::TranslationGizmo(Ogre::SceneNode* linkNode, const Ogre::String &name, Ogre::Real scale, bool leftHandCs)
    : m_pXaxis(nullptr), m_pYaxis(nullptr), m_pZaxis(nullptr), mFade(0.4f), mHighlighted(false)
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

    GizmoAxisHelpers::forEachAxis(m_pXaxis, m_pYaxis, m_pZaxis,
                                  [](Ogre::ManualObject* axis) {
                                      axis->setRenderQueueGroup(ZORDER_OVERLAY);
                                  }); // ensure Depth Check is Off in the material

    setQueryFlags(0);

    GizmoAxisHelpers::forEachAxis(m_pXaxis, m_pYaxis, m_pZaxis,
                                  [linkNode](Ogre::ManualObject* axis) {
                                      linkNode->attachObject(axis);
                                  });
}

TranslationGizmo::~TranslationGizmo()
{
    Ogre::SceneManager* pSceneMgr = m_pXaxis->getParentSceneNode()->getCreator();
    //TODO unload material ??

    GizmoAxisHelpers::forEachAxis(m_pXaxis, m_pYaxis, m_pZaxis,
                                  [pSceneMgr](Ogre::ManualObject* axis) {
                                      pSceneMgr->destroyManualObject(axis);
                                  });
}

void TranslationGizmo::createXaxis(const Ogre::ColourValue& colour)
{
    // Use solid arrows instead of lines for better mouse interaction
    createSolidXaxis(colour);
}

void TranslationGizmo::createYaxis(const Ogre::ColourValue& colour)
{
    // Use solid arrows instead of lines for better mouse interaction
    createSolidYaxis(colour);
}

void TranslationGizmo::createZaxis(const Ogre::ColourValue& colour)
{
    // Use solid arrows instead of lines for better mouse interaction
    createSolidZaxis(colour);
}

void TranslationGizmo::createSolidXaxis(const Ogre::ColourValue& colour)
{
    float thickness = mScale / mSolidThickness;
    // Negative shaft length so the arrow visually points toward world -X.
    // The viewport camera is set up at world -Z looking toward +Z, which
    // makes world +X appear on screen-left. Pointing geometry toward -X
    // makes the arrow appear on screen-right, matching user expectation.
    // Drag/pick math use UNIT_X internally and already produce screen-
    // consistent motion, so only the visual needs flipping.
    float shaftLength = -mScale * 0.85f;
    float headBaseRadius = thickness * 2.5f; // Arrow head base is 2.5x thicker than shaft

    m_pXaxis->clear();
    m_pXaxis->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_TRIANGLE_LIST);
        m_pXaxis->colour(colour);

        // Arrow shaft - box from origin to shaftLength
        m_pXaxis->position(Ogre::Vector3(0,       thickness,  thickness));
        m_pXaxis->position(Ogre::Vector3(0,      -thickness,  thickness));
        m_pXaxis->position(Ogre::Vector3(shaftLength, -thickness,  thickness));
        m_pXaxis->position(Ogre::Vector3(shaftLength,  thickness,  thickness));

        m_pXaxis->position(Ogre::Vector3(0,       thickness, -thickness));
        m_pXaxis->position(Ogre::Vector3(0,      -thickness, -thickness));
        m_pXaxis->position(Ogre::Vector3(shaftLength, -thickness, -thickness));
        m_pXaxis->position(Ogre::Vector3(shaftLength,  thickness, -thickness));

        // Arrow shaft quads (winding flipped because shaft runs toward -X)
        m_pXaxis->quad(3, 2, 1, 0);
        m_pXaxis->quad(4, 5, 6, 7);
        m_pXaxis->quad(4, 7, 3, 0);
        m_pXaxis->quad(6, 5, 1, 2);
        m_pXaxis->quad(7, 6, 2, 3);
        m_pXaxis->quad(5, 4, 0, 1);

        // Arrow head - pyramid pointing in -X direction
        int headBaseIdx = 8;
        // Base of arrow head (square at shaftLength)
        m_pXaxis->position(Ogre::Vector3(shaftLength,  headBaseRadius,  headBaseRadius));
        m_pXaxis->position(Ogre::Vector3(shaftLength, -headBaseRadius,  headBaseRadius));
        m_pXaxis->position(Ogre::Vector3(shaftLength, -headBaseRadius, -headBaseRadius));
        m_pXaxis->position(Ogre::Vector3(shaftLength,  headBaseRadius, -headBaseRadius));
        // Tip of arrow head (at -mScale)
        int headTipIdx = 12;
        m_pXaxis->position(Ogre::Vector3(-mScale, 0, 0));

        // Arrow head faces (4 triangles forming a pyramid; winding
        // reversed from +X version so outward-facing normals still
        // face away from the shaft axis).
        m_pXaxis->triangle(headBaseIdx+1, headBaseIdx, headTipIdx);
        m_pXaxis->triangle(headBaseIdx+2, headBaseIdx+1, headTipIdx);
        m_pXaxis->triangle(headBaseIdx+3, headBaseIdx+2, headTipIdx);
        m_pXaxis->triangle(headBaseIdx, headBaseIdx+3, headTipIdx);
        // Base quad of arrow head (winding reversed)
        m_pXaxis->quad(headBaseIdx+1, headBaseIdx+2, headBaseIdx+3, headBaseIdx);

    m_pXaxis->end();

    // Re-assert the pickable bbox after the rebuild. `end()` auto-
    // computes a tight bbox from vertex extents, which can differ from
    // the explicit bbox the caller set in createAxis. Re-setting here
    // keeps picking consistent with the flipped-X geometry on every
    // hover rebuild.
    const float bbSize = (mScale / mSolidThickness) * 2.5f;
    m_pXaxis->setBoundingBox(GizmoAxisHelpers::makeAxisBoundingBox(
        GizmoAxisHelpers::Axis::X, -mScale, 0.0f, bbSize));
}

void TranslationGizmo::createSolidYaxis(const Ogre::ColourValue& colour)
{
    float thickness = mScale / mSolidThickness;
    float shaftLength = mScale * 0.85f; // Arrow shaft ends at 85% to make room for arrow head
    float headBaseRadius = thickness * 2.5f; // Arrow head base is 2.5x thicker than shaft

    m_pYaxis->clear();
    m_pYaxis->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_TRIANGLE_LIST);
        m_pYaxis->colour(colour);

        // Arrow shaft - box from origin to shaftLength
        m_pYaxis->position(Ogre::Vector3( thickness, 0,       thickness));
        m_pYaxis->position(Ogre::Vector3( thickness, shaftLength,  thickness));
        m_pYaxis->position(Ogre::Vector3(-thickness, shaftLength,  thickness));
        m_pYaxis->position(Ogre::Vector3(-thickness, 0,       thickness));

        m_pYaxis->position(Ogre::Vector3( thickness, 0,      -thickness));
        m_pYaxis->position(Ogre::Vector3( thickness, shaftLength, -thickness));
        m_pYaxis->position(Ogre::Vector3(-thickness, shaftLength, -thickness));
        m_pYaxis->position(Ogre::Vector3(-thickness, 0,      -thickness));

        // Arrow shaft quads
        m_pYaxis->quad(0, 1, 2, 3);
        m_pYaxis->quad(7, 6, 5, 4);
        m_pYaxis->quad(1, 0, 4, 5);
        m_pYaxis->quad(3, 2, 6, 7);
        m_pYaxis->quad(2, 1, 5, 6);
        m_pYaxis->quad(0, 3, 7, 4);

        // Arrow head - pyramid pointing in +Y direction
        int headBaseIdx = 8;
        // Base of arrow head (square at shaftLength)
        m_pYaxis->position(Ogre::Vector3( headBaseRadius, shaftLength,  headBaseRadius));
        m_pYaxis->position(Ogre::Vector3(-headBaseRadius, shaftLength,  headBaseRadius));
        m_pYaxis->position(Ogre::Vector3(-headBaseRadius, shaftLength, -headBaseRadius));
        m_pYaxis->position(Ogre::Vector3( headBaseRadius, shaftLength, -headBaseRadius));
        // Tip of arrow head (at mScale)
        int headTipIdx = 12;
        m_pYaxis->position(Ogre::Vector3(0, mScale, 0));

        // Arrow head faces (4 triangles forming a pyramid)
        m_pYaxis->triangle(headBaseIdx, headBaseIdx+1, headTipIdx);     // Right face
        m_pYaxis->triangle(headBaseIdx+1, headBaseIdx+2, headTipIdx);   // Bottom face
        m_pYaxis->triangle(headBaseIdx+2, headBaseIdx+3, headTipIdx);    // Left face
        m_pYaxis->triangle(headBaseIdx+3, headBaseIdx, headTipIdx);      // Top face
        // Base quad of arrow head
        m_pYaxis->quad(headBaseIdx, headBaseIdx+3, headBaseIdx+2, headBaseIdx+1);

    m_pYaxis->end();
}

void TranslationGizmo::createSolidZaxis(const Ogre::ColourValue& colour)
{
    float thickness = mScale / mSolidThickness;
    Ogre::Real z = 1.0f;

    if(mLeftHandCs)
       z = -1.0f;

    float shaftLength = mScale * 0.85f; // Arrow shaft ends at 85% to make room for arrow head
    float headLength = mScale * 0.15f;  // Arrow head is 15% of total length
    float headBaseRadius = thickness * 2.5f; // Arrow head base is 2.5x thicker than shaft

    m_pZaxis->clear();
    m_pZaxis->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_TRIANGLE_LIST);
        m_pZaxis->colour(colour);

        // Arrow shaft - box from origin to shaftLength
        m_pZaxis->position(Ogre::Vector3( thickness,  thickness,        0));
        m_pZaxis->position(Ogre::Vector3( thickness,  thickness, z*shaftLength));
        m_pZaxis->position(Ogre::Vector3( thickness, -thickness, z*shaftLength));
        m_pZaxis->position(Ogre::Vector3( thickness, -thickness,        0));

        m_pZaxis->position(Ogre::Vector3(-thickness,  thickness,        0));
        m_pZaxis->position(Ogre::Vector3(-thickness,  thickness, z*shaftLength));
        m_pZaxis->position(Ogre::Vector3(-thickness, -thickness, z*shaftLength));
        m_pZaxis->position(Ogre::Vector3(-thickness, -thickness,        0));

        // Arrow shaft quads
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

        // Arrow head - pyramid pointing in +Z or -Z direction
        int headBaseIdx = 8;
        // Base of arrow head (square at shaftLength)
        m_pZaxis->position(Ogre::Vector3( headBaseRadius,  headBaseRadius, z*shaftLength));
        m_pZaxis->position(Ogre::Vector3(-headBaseRadius,  headBaseRadius, z*shaftLength));
        m_pZaxis->position(Ogre::Vector3(-headBaseRadius, -headBaseRadius, z*shaftLength));
        m_pZaxis->position(Ogre::Vector3( headBaseRadius, -headBaseRadius, z*shaftLength));
        // Tip of arrow head (at z*mScale)
        int headTipIdx = 12;
        m_pZaxis->position(Ogre::Vector3(0, 0, z*mScale));

        // Arrow head faces (4 triangles forming a pyramid)
        if(mLeftHandCs)
        {
            m_pZaxis->triangle(headBaseIdx+1, headBaseIdx, headTipIdx);     // Right face
            m_pZaxis->triangle(headBaseIdx+2, headBaseIdx+1, headTipIdx);   // Bottom face
            m_pZaxis->triangle(headBaseIdx+3, headBaseIdx+2, headTipIdx);    // Left face
            m_pZaxis->triangle(headBaseIdx, headBaseIdx+3, headTipIdx);      // Top face
            // Base quad of arrow head
            m_pZaxis->quad(headBaseIdx+1, headBaseIdx, headBaseIdx+3, headBaseIdx+2);
        }
        else
        {
            m_pZaxis->triangle(headBaseIdx, headBaseIdx+1, headTipIdx);     // Right face
            m_pZaxis->triangle(headBaseIdx+1, headBaseIdx+2, headTipIdx);   // Bottom face
            m_pZaxis->triangle(headBaseIdx+2, headBaseIdx+3, headTipIdx);    // Left face
            m_pZaxis->triangle(headBaseIdx+3, headBaseIdx, headTipIdx);      // Top face
            // Base quad of arrow head
            m_pZaxis->quad(headBaseIdx, headBaseIdx+3, headBaseIdx+2, headBaseIdx+1);
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
    GizmoAxisHelpers::forEachAxis(m_pXaxis, m_pYaxis, m_pZaxis,
                                  [visible](Ogre::ManualObject* axis) {
                                      axis->setVisible(visible);
                                  });
}

void TranslationGizmo::setQueryFlags(Ogre::uint32 flags)
{
    GizmoAxisHelpers::forEachAxis(m_pXaxis, m_pYaxis, m_pZaxis,
                                  [flags](Ogre::ManualObject* axis) {
                                      axis->setQueryFlags(flags);
                                  });
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

    // Update bounding boxes for the 3D arrows with arrow heads
    float bbSize = (mScale / mSolidThickness) * 2.5f; // Account for arrow head thickness

    GizmoAxisHelpers::forEachAxisIndexed(m_pXaxis, m_pYaxis, m_pZaxis,
                                         [this, bbSize](GizmoAxisHelpers::Axis axis, Ogre::ManualObject* axisObject) {
                                             // The X geometry is drawn from 0 toward -mScale (see
                                             // createSolidXaxis) to match the camera's view flip, so
                                             // its bbox must mirror the same [-mScale, 0] extent —
                                             // otherwise the visible arrow and the pickable region
                                             // end up on opposite sides of the origin.
                                             // Flip the bbox to the negative range for the X
                                             // axis (geometry flipped for the camera's view
                                             // flip) and for Z under a left-handed coord
                                             // system (legacy viewport option).
                                             const bool flipAxis =
                                                 axis == GizmoAxisHelpers::Axis::X
                                                 || (axis == GizmoAxisHelpers::Axis::Z && mLeftHandCs);
                                             const Ogre::Real axisMin = flipAxis ? -mScale : 0.0f;
                                             const Ogre::Real axisMax = flipAxis ? 0.0f : mScale;
                                             axisObject->setBoundingBox(
                                                 GizmoAxisHelpers::makeAxisBoundingBox(axis, axisMin, axisMax, bbSize));
                                         });

    mHighlighted = false;
}

Ogre::Vector3 TranslationGizmo::highlightAxis(const Ogre::MovableObject* obj)
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
