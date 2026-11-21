#include "SamBodyHands.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace SamBody::Hands {

QRectF handBoxInPhoto(const CropGeometry& body, const std::array<float, 4>& box)
{
    if (!body.valid || !(body.affine[0] > 0.0f)) return {};
    for (float v : box)
        if (!std::isfinite(v)) return {};
    const double a = body.affine[0];
    const double cx = (box[0] * kCropSize - body.affine[2]) / a;
    const double cy = (box[1] * kCropSize - body.affine[5]) / a;
    const double edge = std::max(box[2], box[3]) * kCropSize / a;
    if (!(edge > 0.0)) return {};
    return QRectF(cx - edge * 0.5, cy - edge * 0.5, edge, edge);
}

QRectF mirrorBox(const QRectF& box, int photoW)
{
    // x1' = W - x2 - 1, x2' = W - x1 - 1
    const double x1 = photoW - box.right() - 1.0;
    return QRectF(x1, box.top(), box.width(), box.height());
}

QPointF mirrorPoint(const QPointF& p, int photoW)
{
    return QPointF(photoW - p.x() - 1.0, p.y());
}

QPointF photoToCrop(const CropGeometry& geo, const QPointF& p)
{
    const double x = geo.affine[0] * p.x() + geo.affine[1] * p.y() + geo.affine[2];
    const double y = geo.affine[3] * p.x() + geo.affine[4] * p.y() + geo.affine[5];
    return QPointF(x / kCropSize - 0.5, y / kCropSize - 0.5);
}

bool keypointsInside(const std::vector<float>& kp2dCropped)
{
    if (kp2dCropped.empty()) return false;
    return std::all_of(kp2dCropped.begin(), kp2dCropped.end(),
                       [](float v) { return std::isfinite(v) && std::fabs(v) < kKeypointLimit; });
}

QPointF keypoint(const std::vector<float>& kp2d, int index)
{
    const size_t i = static_cast<size_t>(index) * 2;
    if (index < 0 || i + 1 >= kp2d.size()) {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        return QPointF(nan, nan);
    }
    return QPointF(kp2d[i], kp2d[i + 1]);
}

bool wristClose(const QPointF& handWrist, const QPointF& bodyWrist, double boxEdge)
{
    if (!(boxEdge > 0.0)) return false;
    const double d = std::hypot(handWrist.x() - bodyWrist.x(), handWrist.y() - bodyWrist.y()) / boxEdge;
    return std::isfinite(d) && d < kWristDistance;
}

std::vector<Prompt> wristElbowPrompts(const CropGeometry& body, const QPointF& rightWrist, const QPointF& leftWrist,
                                      const QPointF& rightElbow, const QPointF& leftElbow, const std::array<bool, 2>& valid)
{
    struct In { QPointF p; int label; bool sideOk; };
    const In in[4] = {{rightWrist, kRightWristKp, valid[1]}, {leftWrist, kLeftWristKp, valid[0]},
                      {rightElbow, kRightElbowKp, valid[1]}, {leftElbow, kLeftElbowKp, valid[0]}};
    std::vector<Prompt> out;
    for (const In& i : in) {
        const QPointF c = photoToCrop(body, i.p);
        // Meta keeps a prompt when -0.5 < x, y < 0.5 (strict) and its hand passed every test
        if (!i.sideOk || !std::isfinite(c.x()) || !std::isfinite(c.y())) continue;
        if (!(c.x() > -0.5 && c.x() < 0.5 && c.y() > -0.5 && c.y() < 0.5)) continue;
        Prompt p;
        p.x = static_cast<float>(std::clamp(c.x() + 0.5, 0.0, 1.0));
        p.y = static_cast<float>(std::clamp(c.y() + 0.5, 0.0, 1.0));
        p.label = static_cast<float>(i.label);
        out.push_back(p);
    }
    return out;
}

} // namespace SamBody::Hands
