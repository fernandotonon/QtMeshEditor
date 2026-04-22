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
-----------------------------------------------------------------------------------
*/

#ifndef BEVEL_GIZMO_H
#define BEVEL_GIZMO_H

#include <OgreVector.h>

namespace Ogre {
    class SceneNode;
    class SceneManager;
    class ManualObject;
    class Ray;
    class MovableObject;
}

/**
 * @brief Single-axis drag gizmo used to tune the bevel width interactively.
 *
 * Draws a short shaft with a cube handle at the top. Attached to the world
 * root; its origin + axis direction are set by the controller so the shaft
 * points along the averaged surface normal of the beveled region.
 *
 * The gizmo itself doesn't store width — it only knows about world-space
 * position, axis, and scale. The controller calls distanceAlongAxis(ray)
 * during drag to compute a new width from the ray-to-axis intersection.
 */
class BevelGizmo
{
public:
    BevelGizmo(Ogre::SceneManager* sceneMgr, const Ogre::String& name = "BevelGizmo");
    ~BevelGizmo();

    BevelGizmo(const BevelGizmo&) = delete;
    BevelGizmo& operator=(const BevelGizmo&) = delete;

    /// Position the gizmo in world space and align its shaft with `axis`.
    void setAxis(const Ogre::Vector3& origin, const Ogre::Vector3& axis);

    /// Slide the handle cube along the shaft by `offset` local units from
    /// the shaft base. Used during drag so the visible handle tracks the
    /// current bevel width.
    void setHandleOffset(float offset);

    /// Adjust the node's scale so the gizmo keeps a roughly constant pixel
    /// footprint regardless of camera distance. Call each frame with the
    /// active viewport's camera.
    void updateScreenSpaceScale(const Ogre::Camera* camera);

    /// Show/hide.
    void setVisible(bool visible);
    bool isVisible() const;

    /// Scale the gizmo's visible size (useful for camera distance adjustment).
    void setScale(float scale);

    /// True if `obj` is this gizmo's handle (for picking hit-tests).
    bool isHandle(const Ogre::MovableObject* obj) const;

    /// Project `ray` onto the gizmo's axis line through `origin` and return
    /// the signed distance along the axis from origin to the closest point.
    /// Returns 0 if the ray is parallel to the axis.
    float distanceAlongAxis(const Ogre::Ray& ray) const;

    Ogre::Vector3 origin() const { return m_origin; }
    Ogre::Vector3 axis() const { return m_axis; }

private:
    Ogre::SceneManager* m_sceneMgr = nullptr;
    Ogre::SceneNode* m_node = nullptr;      ///< Position + orientation.
    Ogre::SceneNode* m_shaftNode = nullptr; ///< Child whose Y-scale stretches the shaft.
    Ogre::SceneNode* m_handleNode = nullptr;///< Child holding the picking cube.
    Ogre::ManualObject* m_shaft = nullptr;  ///< Line.
    Ogre::ManualObject* m_handle = nullptr; ///< Cube at top (pickable).
    Ogre::Vector3 m_origin = Ogre::Vector3::ZERO;
    Ogre::Vector3 m_axis = Ogre::Vector3::UNIT_Y;
    float m_scale = 1.0f;

    void buildGeometry(const Ogre::String& name);
};

#endif // BEVEL_GIZMO_H
