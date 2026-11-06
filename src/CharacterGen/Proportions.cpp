#include "Proportions.h"

#include <algorithm>

namespace CharacterGen {

void Proportions::clamp()
{
    for (float* f : {&height, &legs, &arms, &torso, &shoulders, &head}) *f = std::clamp(*f, 0.6f, 1.6f);
}

ProportionFrame applyProportions(const Assets& a, const Proportions& pIn, std::vector<std::array<float, 3>>& gt, std::vector<float>& positions)
{
    ProportionFrame fr;
    Proportions p = pIn;
    p.clamp();
    const size_t J = a.jointParents.size();
    const size_t V = positions.size() / 3;
    if (p.isDefault() || gt.size() != J || a.jointNames.size() != int(J) || a.skinStart.size() != V + 1) return fr;

    // per joint: inherited uniform scale (own) and bone-length factor
    std::vector<float> own(J, 1.0f), length(J, 1.0f);
    for (size_t j = 0; j < J; ++j) {
        const QString& n = a.jointNames[int(j)];
        if (n == QLatin1String("body_world") || (n == QLatin1String("root") && a.jointParents[j] < 0)) own[j] = p.height;
        else if (n == QLatin1String("root")) own[j] = 1.0f;   // (a root under body_world: the height sits on body_world)
        if (n == QLatin1String("l_upleg") || n == QLatin1String("r_upleg")) own[j] = p.legs;
        if (n == QLatin1String("l_uparm") || n == QLatin1String("r_uparm")) own[j] = p.arms;
        if (n == QLatin1String("c_head")) own[j] = p.head;
        if (n == QLatin1String("c_spine1") || n == QLatin1String("c_spine2") || n == QLatin1String("c_spine3")) length[j] = p.torso;
        if (n == QLatin1String("l_clavicle") || n == QLatin1String("r_clavicle")) length[j] = p.shoulders;
    }
    // when body_world exists the height sits on it and root must not repeat it
    bool haveWorld = false;
    for (size_t j = 0; j < J; ++j) if (a.jointNames[int(j)] == QLatin1String("body_world")) haveWorld = true;
    if (haveWorld) for (size_t j = 0; j < J; ++j) if (a.jointNames[int(j)] == QLatin1String("root")) own[j] = 1.0f;

    fr.oldPos = gt;
    fr.newPos = gt;
    fr.scale.assign(J, 1.0f);
    for (size_t j = 0; j < J; ++j) {
        const int par = a.jointParents[j];
        const float inherited = par >= 0 ? fr.scale[size_t(par)] : 1.0f;
        fr.scale[j] = inherited * own[j];
        if (par < 0) { fr.newPos[j] = gt[j]; continue; }
        const auto& pp = gt[size_t(par)];
        for (size_t k = 0; k < 3; ++k) fr.newPos[j][k] = fr.newPos[size_t(par)][k] + inherited * length[j] * (gt[j][k] - pp[k]);
    }

    // the vertices follow by linear blend skinning: v' = sum w_k (newPos_k + scale_k (v - oldPos_k))
    float oldMin = 1e30f, newMin = 1e30f;
    std::vector<float> out(positions.size());
    for (size_t v = 0; v < V; ++v) {
        double acc[3] = {0, 0, 0}, wsum = 0.0;
        for (uint32_t k = a.skinStart[v]; k < a.skinStart[v + 1]; ++k) {
            const int j = a.skinJoint[k];
            if (j < 0 || size_t(j) >= J) continue;
            const double w = a.skinWeight[k];
            wsum += w;
            for (size_t c = 0; c < 3; ++c) acc[c] += w * (double(fr.newPos[size_t(j)][c]) + double(fr.scale[size_t(j)]) * (double(positions[v * 3 + c]) - double(fr.oldPos[size_t(j)][c])));
        }
        for (size_t c = 0; c < 3; ++c) out[v * 3 + c] = wsum > 1e-9 ? float(acc[c] / wsum) : positions[v * 3 + c];
        oldMin = std::min(oldMin, positions[v * 3 + 1]);
        newMin = std::min(newMin, out[v * 3 + 1]);
    }
    fr.lift = V ? oldMin - newMin : 0.0f;   // keep the lowest vertex where it was (the feet stay on the floor)
    for (size_t v = 0; v < V; ++v) out[v * 3 + 1] += fr.lift;
    for (auto& q : fr.newPos) q[1] += fr.lift;
    positions = std::move(out);
    gt = fr.newPos;
    fr.active = true;
    return fr;
}

}  // namespace CharacterGen
