/*
-----------------------------------------------------------------------------------
This source file is part of QtMeshEditor.

Paint v2 Slice F (#549) — decal placement state machine.

A world-anchored oriented quad the user places on the mesh surface, then
translate / rotate / scale via viewport handles; on commit it produces the
ProjectionPainter inputs (an orthographic View framing the quad + the decal
image with a soft-edge alpha) so the decal is rasterized onto the mesh into a
new layer.

Pure data (no Ogre scene / GL / Qt widgets): the controller supplies
screen->world resolution; this class only does the quad geometry + state, so it
is fully unit-testable headlessly.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#ifndef DECALSESSION_H
#define DECALSESSION_H

#include "ProjectionPainter.h"

#include <QImage>

#include <OgreVector2.h>
#include <OgreVector3.h>

class DecalSession
{
public:
    enum class State { Idle, Placing, Editing };
    /// Which part of the rectangle a viewport click grabbed.
    enum class Handle { None, Body, RotateCorner, ScaleEdge };

    /// World-space oriented quad. tangentU/V lengths are the half-width/height,
    /// so the four corners are center ± tangentU ± tangentV. `normal` is the
    /// projection direction (into the surface).
    struct Rect {
        Ogre::Vector3 center;
        Ogre::Vector3 normal   = Ogre::Vector3::UNIT_Z;
        Ogre::Vector3 tangentU = Ogre::Vector3::UNIT_X;  // half-width axis
        Ogre::Vector3 tangentV = Ogre::Vector3::UNIT_Y;  // half-height axis
    };

    /// The ProjectionPainter inputs a commit produces.
    struct CommitInputs {
        QImage                  source;   ///< decal image with soft-edge alpha
        ProjectionPainter::View view;     ///< orthographic frame around the quad
    };

    void begin(const QImage& decalImage);
    /// Anchor the rect on the surface: center=worldHit, normal=worldNormal,
    /// oriented so tangentV aligns with `cameraUp` projected onto the plane.
    /// `halfSize` sets the initial half-extent (world units). Placing -> Editing.
    void place(const Ogre::Vector3& worldHit, const Ogre::Vector3& worldNormal,
               const Ogre::Vector3& cameraUp, float halfSize);
    void cancel();

    State state() const { return m_state; }
    bool  active() const { return m_state != State::Idle; }
    const Rect& rect() const { return m_rect; }
    const QImage& image() const { return m_image; }

    /// Classify a point given in rect-LOCAL normalised coords (u,v in [-1,1],
    /// where ±1 is an edge): corner zone -> RotateCorner, edge zone -> ScaleEdge,
    /// interior -> Body, outside -> None.
    Handle hitTest(float u, float v) const;

    // --- edits (Editing state) ---
    void translate(const Ogre::Vector3& worldDelta);
    void rotate(float deltaRad);          ///< about the rect normal
    void scale(float su, float sv);       ///< multiply half-extents (clamped > 0)

    /// Project a world point onto the rect plane and return its normalised
    /// rect-local coords (u,v in tangentU/tangentV units; ±1 = edge).
    Ogre::Vector2 worldToRectUv(const Ogre::Vector3& worldP) const;
    /// The four world corners: [--, +-, ++, -+] in (u,v) sign order.
    void corners(Ogre::Vector3 out[4]) const;

    /// Build the ProjectionPainter inputs (ortho View + soft-edge source image).
    /// `softEdge` is the border feather fraction (0..1). Empty on Idle.
    CommitInputs buildCommit(float softEdge) const;

private:
    State  m_state = State::Idle;
    Rect   m_rect;
    QImage m_image;
};

#endif // DECALSESSION_H
