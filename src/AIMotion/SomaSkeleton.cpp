#include "SomaSkeleton.h"

#include <algorithm>
#include <cstring>

namespace AIMotion {

namespace {

bool endsWith(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Body joints, side-free part. Exact names only: a near miss must stay Unknown.
const std::pair<const char*, const char*> kCentre[] = {
    {"Hips", "Hips"}, {"Spine1", "Spine"}, {"Spine2", "Spine1"}, {"Chest", "Spine2"},
    {"Neck1", "Neck"}, {"Neck2", "Neck1"}, {"Head", "Head"},
};
const std::pair<const char*, const char*> kLimb[] = {
    {"Shoulder", "Shoulder"}, {"Arm", "Arm"}, {"ForeArm", "ForeArm"}, {"Hand", "Hand"},
    {"Leg", "UpLeg"}, {"Shin", "Leg"}, {"Foot", "Foot"}, {"ToeBase", "ToeBase"},
};
const char* kLongFingers[] = {"Index", "Middle", "Ring", "Pinky"};

bool splitSide(const std::string& n, std::string* side, std::string* part)
{
    for (const char* s : {"Left", "Right"}) {
        const size_t len = std::strlen(s);
        if (n.size() > len && n.compare(0, len, s) == 0) {
            *side = s;
            *part = n.substr(len);
            return true;
        }
    }
    return false;
}

} // namespace

SomaRole somaJointRole(const std::string& n)
{
    if (n == "HeadEnd" || endsWith(n, "End")) return SomaRole::EndEffector;
    if (n == "Jaw" || n == "LeftEye" || n == "RightEye") return SomaRole::Facial;
    std::string side, part;
    if (splitSide(n, &side, &part) && part.rfind("Hand", 0) == 0 && part.size() > 4) {
        const std::string finger = part.substr(4);
        for (const char* f : kLongFingers)
            if (finger == std::string(f) + "1") return SomaRole::Metacarpal;
    }
    return somaStandardName(n).empty() ? SomaRole::Unknown : SomaRole::Mapped;
}

std::string somaStandardName(const std::string& n)
{
    for (const auto& c : kCentre)
        if (n == c.first) return c.second;
    std::string side, part;
    if (!splitSide(n, &side, &part)) return {};
    for (const auto& l : kLimb)
        if (part == l.first) return side + l.second;
    if (part.rfind("Hand", 0) != 0 || part.size() < 6) return {};
    const std::string finger = part.substr(4, part.size() - 5);
    const char digit = part.back();
    if (finger == "Thumb" && digit >= '1' && digit <= '3') return side + "HandThumb" + digit;
    for (const char* f : kLongFingers)
        if (finger == f && digit >= '2' && digit <= '4') return side + "Hand" + f + char(digit - 1);
    return {};
}

const char* somaRoleId(SomaRole r)
{
    switch (r) {
    case SomaRole::Mapped: return "mapped";
    case SomaRole::EndEffector: return "end";
    case SomaRole::Facial: return "facial";
    case SomaRole::Metacarpal: return "metacarpal";
    case SomaRole::Unknown: break;
    }
    return "unknown";
}

bool looksLikeSoma(const std::vector<std::string>& names)
{
    for (const char* must : {"Hips", "Spine1", "Chest", "LeftShin", "RightShin", "LeftLeg", "RightLeg"})
        if (std::find(names.begin(), names.end(), must) == names.end()) return false;
    return true;
}

} // namespace AIMotion
