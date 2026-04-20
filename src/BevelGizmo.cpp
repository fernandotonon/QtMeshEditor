/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
MIT License — see BevelGizmo.h
-----------------------------------------------------------------------------------
*/

#include "BevelGizmo.h"
#include "GlobalDefinitions.h"

#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreManualObject.h>
#include <OgreRay.h>
#include <OgreQuaternion.h>
#include <OgreCamera.h>
#include <cmath>

namespace
{
    // Axis-aligned cube centered at origin with half-side s, emitted into a
    // triangle-list ManualObject. Same winding convention as ScaleGizmo's
    // createCube.
    void emitCube(Ogre::ManualObject* obj, const Ogre::Vector3& center, float s,
                  const Ogre::ColourValue& colour)
    {
        float cx = center.x, cy = center.y, cz = center.z;
        int base = obj->getCurrentVertexCount();

        auto vert = [&](float x, float y, float z) {
            obj->position(Ogre::Vector3(x, y, z));
            obj->colour(colour);
        };
        vert(cx - s, cy + s, cz + s);
        vert(cx - s, cy - s, cz + s);
        vert(cx + s, cy - s, cz + s);
        vert(cx + s, cy + s, cz + s);
        vert(cx - s, cy + s, cz - s);
        vert(cx - s, cy - s, cz - s);
        vert(cx + s, cy - s, cz - s);
        vert(cx + s, cy + s, cz - s);

        obj->quad(base + 0, base + 1, base + 2, base + 3);
        obj->quad(base + 7, base + 6, base + 5, base + 4);
        obj->quad(base + 0, base + 3, base + 7, base + 4);
        obj->quad(base + 2, base + 1, base + 5, base + 6);
        obj->quad(base + 3, base + 2, base + 6, base + 7);
        obj->quad(base + 1, base + 0, base + 4, base + 5);
    }
}

BevelGizmo::BevelGizmo(Ogre::SceneManager* sceneMgr, const Ogre::String& name)
    : m_sceneMgr(sceneMgr)
{
    if (!sceneMgr) return;

    m_node = sceneMgr->getRootSceneNode()->createChildSceneNode(name + "_Node");
    m_shaftNode = m_node->createChildSceneNode(name + "_Shaft");
    m_handleNode = m_node->createChildSceneNode(name + "_Handle");

    buildGeometry(name);

    m_shaftNode->attachObject(m_shaft);
    m_handleNode->attachObject(m_handle);
    setVisible(false);
}

BevelGizmo::~BevelGizmo()
{
    if (!m_sceneMgr) return;

    if (m_handle) {
        if (m_handleNode) m_handleNode->detachObject(m_handle);
        m_sceneMgr->destroyManualObject(m_handle);
    }
    if (m_shaft) {
        if (m_shaftNode) m_shaftNode->detachObject(m_shaft);
        m_sceneMgr->destroyManualObject(m_shaft);
    }
    if (m_handleNode) {
        m_handleNode->getParent()->removeChild(m_handleNode);
        m_sceneMgr->destroySceneNode(m_handleNode);
    }
    if (m_shaftNode) {
        m_shaftNode->getParent()->removeChild(m_shaftNode);
        m_sceneMgr->destroySceneNode(m_shaftNode);
    }
    if (m_node) {
        m_node->getParent()->removeChild(m_node);
        m_sceneMgr->destroySceneNode(m_node);
    }
}

void BevelGizmo::buildGeometry(const Ogre::String& name)
{
    const Ogre::ColourValue shaftColour(0.1f, 0.9f, 0.4f, 1.0f);
    const Ogre::ColourValue handleColour(1.0f, 0.85f, 0.1f, 1.0f); // yellow-orange — stands out against green
    const float shaftLength = 0.4f;
    const float handleHalfSize = 0.06f;

    // Shaft: line list along +Y in local space (the parent node's rotation
    // remaps this to the target axis in world space).
    m_shaft = m_sceneMgr->createManualObject(name + "_Shaft");
    m_shaft->setDynamic(false);
    m_shaft->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_LINE_LIST);
        m_shaft->position(Ogre::Vector3::ZERO); m_shaft->colour(shaftColour);
        m_shaft->position(Ogre::Vector3(0, shaftLength, 0)); m_shaft->colour(shaftColour);
    m_shaft->end();
    // Explicit bounds so scene queries / culling don't drop it.
    m_shaft->setBoundingBox(Ogre::AxisAlignedBox(
        -0.01f, 0.0f, -0.01f, 0.01f, shaftLength, 0.01f));
    m_shaft->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY);
    m_shaft->setQueryFlags(0); // shaft is not pickable; only the handle is

    // Handle cube at tip. Query-flag-pickable; the controller asks
    // isHandle(obj) to identify hits.
    m_handle = m_sceneMgr->createManualObject(name + "_Handle_Geom");
    m_handle->setDynamic(false);
    m_handle->begin(GUI_MATERIAL_NAME, Ogre::RenderOperation::OT_TRIANGLE_LIST);
        emitCube(m_handle, Ogre::Vector3::ZERO, handleHalfSize, handleColour);
    m_handle->end();
    m_handle->setBoundingBox(Ogre::AxisAlignedBox(
        -handleHalfSize, -handleHalfSize, -handleHalfSize,
         handleHalfSize,  handleHalfSize,  handleHalfSize));
    m_handle->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY);
    m_handle->setQueryFlags(0xFFFFFFFF);

    // Place handle at top of shaft (in local Y space).
    m_handleNode->setPosition(0, shaftLength, 0);
}

void BevelGizmo::setAxis(const Ogre::Vector3& origin, const Ogre::Vector3& axis)
{
    m_origin = origin;
    Ogre::Vector3 a = axis;
    if (a.length() < 1e-6f)
        a = Ogre::Vector3::UNIT_Y;
    else
        a.normalise();
    m_axis = a;

    if (!m_node) return;
    m_node->setPosition(origin);
    // Rotate local +Y to point along `m_axis`.
    m_node->setOrientation(Ogre::Vector3::UNIT_Y.getRotationTo(m_axis));
}

void BevelGizmo::setHandleOffset(float offset)
{
    if (m_handleNode) m_handleNode->setPosition(0, offset, 0);
    // Stretch just the shaft node so the line's tip lands at the handle.
    // Shaft geometry is authored at length 0.4 in local Y; scaling Y by
    // offset/0.4 puts its tip at (0, offset, 0) in the parent's frame.
    if (m_shaftNode) {
        float yScale = (offset > 1e-6f) ? (offset / 0.4f) : 1e-6f;
        m_shaftNode->setScale(1.0f, yScale, 1.0f);
    }
}

void BevelGizmo::setVisible(bool visible)
{
    if (m_node) m_node->setVisible(visible, true);
}

bool BevelGizmo::isVisible() const
{
    // m_node has no directly attached objects; the shaft and handle are
    // attached to their own child nodes. Query the ManualObjects themselves
    // so the reported visibility matches the cascaded setVisible() call.
    if (m_shaft && m_shaft->isVisible()) return true;
    if (m_handle && m_handle->isVisible()) return true;
    return false;
}

void BevelGizmo::setScale(float scale)
{
    m_scale = (scale > 1e-6f) ? scale : 1.0f;
    if (m_node) m_node->setScale(m_scale, m_scale, m_scale);
}

void BevelGizmo::updateScreenSpaceScale(const Ogre::Camera* camera)
{
    if (!camera || !m_node) return;
    // Distance from camera to gizmo origin. For perspective cameras this
    // scales the gizmo's world size linearly with distance, which produces
    // a constant angular (pixel) size. An arbitrary coefficient (0.12) was
    // chosen so the gizmo looks similar in pixel size to the old fixed
    // 0.1-length shaft when the camera is ~1 unit from the origin.
    float dist = (camera->getDerivedPosition() - m_origin).length();
    if (dist < 1e-4f) dist = 1e-4f;
    float s = dist * 0.12f;
    m_node->setScale(s, s, s);
    m_scale = s;
}

bool BevelGizmo::isHandle(const Ogre::MovableObject* obj) const
{
    return obj && obj == static_cast<const Ogre::MovableObject*>(m_handle);
}

float BevelGizmo::distanceAlongAxis(const Ogre::Ray& ray) const
{
    // Closest-point projection between two lines: the gizmo axis (origin, m_axis)
    // and the viewer ray. Returns the signed parameter t along the axis for the
    // axis-line's closest point to the ray.
    const Ogre::Vector3& p1 = m_origin;
    const Ogre::Vector3& d1 = m_axis;      // unit
    const Ogre::Vector3& p2 = ray.getOrigin();
    Ogre::Vector3 d2 = ray.getDirection(); // assume unit

    Ogre::Vector3 r = p1 - p2;
    float a = d1.dotProduct(d1);
    float b = d1.dotProduct(d2);
    float c = d2.dotProduct(d2);
    float d = d1.dotProduct(r);
    float e = d2.dotProduct(r);
    float denom = a * c - b * b;
    if (std::abs(denom) < 1e-8f)
        return 0.0f; // parallel
    // t = parameter along axis from p1 toward closest point to ray
    return (b * e - c * d) / denom;
}
