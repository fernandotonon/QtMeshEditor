/*
-----------------------------------------------------------------------------------
This source file is part of QtMeshEditor.

Paint v2 Slice F (#549) — DecalSession implementation.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include "DecalSession.h"

#include <OgreMatrix4.h>
#include <OgreQuaternion.h>
#include <OgreVector4.h>

#include <QColor>

#include <algorithm>
#include <cmath>

void DecalSession::begin(const QImage& decalImage)
{
    m_image = decalImage.isNull()
                  ? QImage()
                  : decalImage.convertToFormat(QImage::Format_RGBA8888);
    m_rect = Rect{};
    m_state = State::Placing;
}

void DecalSession::place(const Ogre::Vector3& worldHit, const Ogre::Vector3& worldNormal,
                         const Ogre::Vector3& cameraUp, float halfSize)
{
    if (m_state == State::Idle) return;
    Ogre::Vector3 n = worldNormal;
    if (n.isZeroLength()) n = Ogre::Vector3::UNIT_Z;
    n.normalise();

    // tangentV = cameraUp projected onto the plane (fallback if parallel to n).
    Ogre::Vector3 up = cameraUp - n * cameraUp.dotProduct(n);
    if (up.isZeroLength()) {
        up = (std::abs(n.dotProduct(Ogre::Vector3::UNIT_Y)) > 0.95f)
                 ? Ogre::Vector3::UNIT_X : Ogre::Vector3::UNIT_Y;
        up = up - n * up.dotProduct(n);
    }
    up.normalise();
    Ogre::Vector3 right = up.crossProduct(n);   // tangentU
    if (right.isZeroLength()) right = Ogre::Vector3::UNIT_X;
    right.normalise();

    if (halfSize <= 0.0f) halfSize = 0.5f;
    // Preserve the source image's aspect ratio: commit maps the WHOLE image onto
    // this rect, so equal U/V extents would stretch any non-square logo/label
    // until the user hand-guessed the correction. `halfSize` stays the V (height)
    // extent and U widens/narrows with the image aspect.
    float aspect = 1.0f;
    if (!m_image.isNull() && m_image.height() > 0)
        aspect = static_cast<float>(m_image.width()) / static_cast<float>(m_image.height());
    if (!std::isfinite(aspect) || aspect <= 0.0f) aspect = 1.0f;
    m_rect.center   = worldHit;
    m_rect.normal   = n;
    m_rect.tangentU = right * (halfSize * aspect);
    m_rect.tangentV = up * halfSize;
    m_state = State::Editing;
}

void DecalSession::cancel()
{
    m_state = State::Idle;
    m_image = QImage();
    m_rect = Rect{};
}

DecalSession::Handle DecalSession::hitTest(float u, float v) const
{
    const float au = std::abs(u), av = std::abs(v);
    if (au > 1.15f || av > 1.15f) return Handle::None;      // clearly outside
    const float cornerBand = 0.75f;   // within 25% of both corners
    if (au >= cornerBand && av >= cornerBand) return Handle::RotateCorner;
    const float edgeBand = 0.8f;      // near one edge only
    if (au >= edgeBand || av >= edgeBand) return Handle::ScaleEdge;
    return Handle::Body;
}

void DecalSession::translate(const Ogre::Vector3& worldDelta)
{
    if (m_state != State::Editing) return;
    m_rect.center += worldDelta;
}

void DecalSession::rotate(float deltaRad)
{
    if (m_state != State::Editing) return;
    const Ogre::Quaternion q(Ogre::Radian(deltaRad), m_rect.normal);
    m_rect.tangentU = q * m_rect.tangentU;
    m_rect.tangentV = q * m_rect.tangentV;
}

void DecalSession::scale(float su, float sv)
{
    if (m_state != State::Editing) return;
    su = std::max(su, 1e-3f);
    sv = std::max(sv, 1e-3f);
    m_rect.tangentU *= su;
    m_rect.tangentV *= sv;
}

Ogre::Vector2 DecalSession::worldToRectUv(const Ogre::Vector3& worldP) const
{
    const Ogre::Vector3 d = worldP - m_rect.center;
    const float lu2 = m_rect.tangentU.squaredLength();
    const float lv2 = m_rect.tangentV.squaredLength();
    const float u = (lu2 > 1e-12f) ? d.dotProduct(m_rect.tangentU) / lu2 : 0.0f;
    const float v = (lv2 > 1e-12f) ? d.dotProduct(m_rect.tangentV) / lv2 : 0.0f;
    return Ogre::Vector2(u, v);
}

void DecalSession::corners(Ogre::Vector3 out[4]) const
{
    out[0] = m_rect.center - m_rect.tangentU - m_rect.tangentV; // (-,-)
    out[1] = m_rect.center + m_rect.tangentU - m_rect.tangentV; // (+,-)
    out[2] = m_rect.center + m_rect.tangentU + m_rect.tangentV; // (+,+)
    out[3] = m_rect.center - m_rect.tangentU + m_rect.tangentV; // (-,+)
}

DecalSession::CommitInputs DecalSession::buildCommit(float softEdge) const
{
    CommitInputs out;
    if (m_state == State::Idle) return out;

    // Soft-edge alpha: feather the image's alpha toward the border so the decal
    // blends onto the surface instead of a hard rectangle cut.
    QImage src = m_image.isNull()
                     ? QImage(1, 1, QImage::Format_RGBA8888)
                     : m_image.convertToFormat(QImage::Format_RGBA8888);
    if (m_image.isNull()) src.fill(QColor(255, 255, 255, 255));
    if (softEdge > 0.0f) {
        const int w = src.width(), h = src.height();
        for (int y = 0; y < h; ++y) {
            uchar* row = src.scanLine(y);
            const float fy = std::clamp(std::min(y, h - 1 - y) / (softEdge * h), 0.0f, 1.0f);
            for (int x = 0; x < w; ++x) {
                const float fx = std::clamp(std::min(x, w - 1 - x) / (softEdge * w), 0.0f, 1.0f);
                const float f = std::min(fx, fy);
                row[x * 4 + 3] = static_cast<uchar>(row[x * 4 + 3] * f);
            }
        }
    }
    out.source = src;

    // Orthographic View framing the quad: build world->clip directly as a
    // change of basis. A world point P maps to NDC (u, v, 0) where
    //   u = dot(P - center, tangentU) / |tangentU|^2   in [-1,1] on the quad,
    //   v = dot(P - center, tangentV) / |tangentV|^2 .
    // projectToViewportUV does the NDC->UV flip, so the quad fills the source.
    const Ogre::Vector3 tU = m_rect.tangentU;
    const Ogre::Vector3 tV = m_rect.tangentV;
    const float invLu2 = tU.squaredLength() > 1e-12f ? 1.0f / tU.squaredLength() : 0.0f;
    const float invLv2 = tV.squaredLength() > 1e-12f ? 1.0f / tV.squaredLength() : 0.0f;
    const Ogre::Vector3 su = tU * invLu2;   // row that yields u
    const Ogre::Vector3 sv = tV * invLv2;   // row that yields v
    // Depth row: signed distance along the normal, scaled small so nothing clips
    // (w stays 1 so projectToViewportUV's behind-test passes for on/near-plane).
    const Ogre::Vector3 sn = m_rect.normal;
    const Ogre::Vector3 c = m_rect.center;

    Ogre::Matrix4 m = Ogre::Matrix4::ZERO;
    m[0][0] = su.x; m[0][1] = su.y; m[0][2] = su.z; m[0][3] = -su.dotProduct(c);
    m[1][0] = sv.x; m[1][1] = sv.y; m[1][2] = sv.z; m[1][3] = -sv.dotProduct(c);
    m[2][0] = sn.x * 1e-3f; m[2][1] = sn.y * 1e-3f; m[2][2] = sn.z * 1e-3f;
    m[2][3] = -sn.dotProduct(c) * 1e-3f;
    m[3][3] = 1.0f;   // w = 1 for every point

    out.view.viewProj = m;
    out.view.camDirection = -m_rect.normal;   // camera looks toward the surface (opposite the outward normal)
    out.view.camPosition = m_rect.center + m_rect.normal * 1.0f;
    return out;
}
