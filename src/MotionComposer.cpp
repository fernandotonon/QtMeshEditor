#include "MotionComposer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace {

// Canonical foot roles (see AnimationMerger's kParentCanon table): 17 = rfoot,
// 21 = lfoot. Seams read worst at the feet — a mismatched stance slides the
// whole character — so they carry double weight in the cut search.
constexpr int kRFoot = 17;
constexpr int kLFoot = 21;

// Connectives that separate timeline steps. Longest-first so "and then" is
// consumed before the bare "and".
const char* const kSeparators[] = {
    " and then ", " after that ", " followed by ", " then ", " finally ",
    " next ", " after ", " and ", ";", ",",
};

// Spelled-out repeat counts. "once" is 1 and needs no entry, but "twice" and
// "thrice" have no digit form a regex would catch.
struct WordCount { const char* word; int n; };
const WordCount kRepeatWords[] = {
    {"twice", 2}, {"two times", 2}, {"thrice", 3}, {"three times", 3},
    {"four times", 4}, {"five times", 5},
};

int parseRepeat(const QString& seg)
{
    const QString s = seg.toLower();
    for (const auto& w : kRepeatWords)
        if (s.contains(QLatin1String(w.word))) return w.n;
    // "N times" / "N x"
    static const QRegularExpression re(
        QStringLiteral("(\\d+)\\s*(?:times|x)\\b"));
    const auto m = re.match(s);
    if (m.hasMatch()) {
        const int n = m.captured(1).toInt();
        if (n > 0) return std::min(n, 16);   // sanity cap
    }
    return 1;
}

float parseDuration(const QString& seg)
{
    static const QRegularExpression re(
        QStringLiteral("(?:for\\s+)?(\\d+(?:\\.\\d+)?)\\s*(?:seconds|second|secs|sec|s)\\b"));
    const auto m = re.match(seg.toLower());
    if (!m.hasMatch()) return 0.0f;
    const float v = m.captured(1).toFloat();
    return (v > 0.0f && v < 120.0f) ? v : 0.0f;
}

// Split on any connective, longest-first, preserving order.
QStringList segment(const QString& prompt)
{
    QStringList parts{prompt};
    for (const char* sep : kSeparators) {
        QStringList next;
        for (const QString& p : parts) {
            const QStringList sub =
                p.split(QLatin1String(sep), Qt::SkipEmptyParts, Qt::CaseInsensitive);
            for (const QString& s : sub) {
                const QString t = s.trimmed();
                if (!t.isEmpty()) next << t;
            }
        }
        parts = next;
    }
    return parts;
}

std::array<float, 4> normalizeQ(std::array<float, 4> q)
{
    const float n = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (n < 1e-8f) return {0.0f, 0.0f, 0.0f, 1.0f};
    return {q[0]/n, q[1]/n, q[2]/n, q[3]/n};
}

} // namespace

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

MotionComposer::Script MotionComposer::parse(const QString& prompt,
                                             const MotionLibrary& lib)
{
    Script out;
    for (const QString& seg : segment(prompt)) {
        const QString action = lib.resolveAction(seg);
        if (action.isEmpty()) {
            out.unresolved.push_back(seg);
            continue;
        }
        Step st;
        st.action = action;
        st.rawText = seg;
        st.repeat = parseRepeat(seg);
        st.durationS = parseDuration(seg);
        out.steps.push_back(std::move(st));
    }
    return out;
}

bool MotionComposer::promptHasMultipleSteps(const QString& prompt)
{
    // Same splitter parse() uses, so the two can never disagree about where a
    // prompt divides — but library-free, so the model path can consult it
    // before deciding whether to load the library at all.
    return segment(prompt).size() > 1;
}

MotionComposer::Script MotionComposer::parseJson(const QByteArray& json,
                                                 const MotionLibrary& lib)
{
    Script out;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    for (const QJsonValue& sv : root.value(QStringLiteral("steps")).toArray()) {
        const QJsonObject so = sv.toObject();
        const QString raw = so.value(QStringLiteral("action")).toString();
        if (raw.isEmpty()) continue;
        const QString action = lib.resolveAction(raw);
        if (action.isEmpty()) {
            out.unresolved.push_back(raw);
            continue;
        }
        Step st;
        st.action = action;
        st.rawText = raw;
        st.repeat = std::clamp(so.value(QStringLiteral("repeat")).toInt(1), 1, 16);
        const double d = so.value(QStringLiteral("duration_s")).toDouble(0.0);
        st.durationS = (d > 0.0 && d < 120.0) ? static_cast<float>(d) : 0.0f;
        out.steps.push_back(std::move(st));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

double MotionComposer::poseDistance(const std::vector<std::array<float, 4>>& a,
                                    const std::vector<std::array<float, 4>>& b)
{
    const size_t n = std::min(a.size(), b.size());
    double sum = 0.0;
    for (size_t j = 0; j < n; ++j) {
        // Geodesic angle between unit quats, sign-insensitive (q and -q are
        // the same rotation).
        double d = std::abs(static_cast<double>(a[j][0]) * b[j][0]
                          + static_cast<double>(a[j][1]) * b[j][1]
                          + static_cast<double>(a[j][2]) * b[j][2]
                          + static_cast<double>(a[j][3]) * b[j][3]);
        d = std::clamp(d, 0.0, 1.0);
        const double ang = 2.0 * std::acos(d);
        const double w = (j == kRFoot || j == kLFoot) ? 2.0 : 1.0;
        sum += w * ang * ang;
    }
    return sum;
}

std::pair<int, int> MotionComposer::bestCut(
    const std::vector<std::vector<std::array<float, 4>>>& a,
    const std::vector<std::vector<std::array<float, 4>>>& b,
    int window)
{
    const int na = static_cast<int>(a.size());
    const int nb = static_cast<int>(b.size());
    if (na == 0 || nb == 0) return {std::max(0, na - 1), 0};
    const int wa = std::clamp(window, 1, na);
    const int wb = std::clamp(window, 1, nb);
    int bestA = na - 1, bestB = 0;
    double best = std::numeric_limits<double>::max();
    for (int i = na - wa; i < na; ++i) {
        for (int j = 0; j < wb; ++j) {
            const double d = poseDistance(a[static_cast<size_t>(i)],
                                          b[static_cast<size_t>(j)]);
            if (d < best) { best = d; bestA = i; bestB = j; }
        }
    }
    return {bestA, bestB};
}

std::array<float, 4> MotionComposer::slerp(const std::array<float, 4>& a,
                                           const std::array<float, 4>& b,
                                           float t)
{
    std::array<float, 4> q0 = normalizeQ(a), q1 = normalizeQ(b);
    float dot = q0[0]*q1[0] + q0[1]*q1[1] + q0[2]*q1[2] + q0[3]*q1[3];
    if (dot < 0.0f) {           // shortest arc
        for (float& c : q1) c = -c;
        dot = -dot;
    }
    if (dot > 0.9995f) {        // nearly parallel — lerp then renormalize
        return normalizeQ({q0[0] + t*(q1[0]-q0[0]), q0[1] + t*(q1[1]-q0[1]),
                           q0[2] + t*(q1[2]-q0[2]), q0[3] + t*(q1[3]-q0[3])});
    }
    const float theta = std::acos(std::clamp(dot, -1.0f, 1.0f));
    const float s = std::sin(theta);
    const float w0 = std::sin((1.0f - t) * theta) / s;
    const float w1 = std::sin(t * theta) / s;
    return normalizeQ({q0[0]*w0 + q1[0]*w1, q0[1]*w0 + q1[1]*w1,
                       q0[2]*w0 + q1[2]*w1, q0[3]*w0 + q1[3]*w1});
}

// ---------------------------------------------------------------------------
// Compose
// ---------------------------------------------------------------------------

MotionComposer::Composition MotionComposer::compose(const Script& script,
                                                    const MotionLibrary& lib,
                                                    int blendFrames)
{
    Composition out;
    if (script.steps.empty()) {
        out.error = QStringLiteral("no recognizable actions in the prompt");
        return out;
    }

    // ---- 1. Compile each step to a concrete frame sequence -----------------
    struct Piece {
        std::vector<std::vector<std::array<float, 4>>> quats;
        std::vector<float> rootY;
        std::vector<std::vector<std::array<float, 4>>> fingers;
        QString action;
    };
    std::vector<Piece> pieces;
    int firstTake = -1;

    for (const Step& st : script.steps) {
        const int idx = lib.pickTake(st.action);
        if (idx < 0) {
            out.error = QStringLiteral("no clip for action '%1'").arg(st.action);
            return out;
        }
        const MotionLibrary::Clip& c = lib.clip(idx);
        if (c.quats.empty()) continue;
        if (firstTake < 0) firstTake = idx;

        // Cut to the requested duration (never extend a take by holding a
        // frozen pose — a stretched still reads as a hang, so a too-short take
        // simply plays its natural length).
        int keep = static_cast<int>(c.quats.size());
        if (st.durationS > 0.0f) {
            const int want = std::max(2, static_cast<int>(
                std::lround(st.durationS * static_cast<double>(c.fps))));
            keep = std::min(keep, want);
        }

        Piece p;
        p.action = st.action;
        const bool haveRootY = c.rootY.size() == c.quats.size();
        // V1 side-channel fingers travel WITH their own take's frames, so a
        // repeated single take keeps them perfectly in step.
        const bool haveFingers = c.fingers.size() == c.quats.size();
        const int reps = std::max(1, st.repeat);
        // #1009: repeat over the clip's LOOP RANGE when it has one, so "wave
        // twice" cycles the wave itself instead of replaying the lead-in and
        // settle each time. Non-loopable takes (one-shots like death/pickup)
        // repeat whole, which is the only sensible reading of "do it twice".
        // The loop slice must fall INSIDE the kept range, or later passes get
        // from > to and contribute nothing — "wave twice" with a short duration
        // silently played once. When the duration cuts before loopStart there
        // is no cycle left to repeat, so fall back to repeating what was kept.
        const bool useLoop = reps > 1 && c.loopable
                             && c.loopEnd > c.loopStart
                             && c.loopEnd < static_cast<int>(c.quats.size())
                             && c.loopStart < keep;
        for (int r = 0; r < reps; ++r) {
            // First pass plays from the top so the action's entry is kept;
            // later passes replay only the cycle.
            const int from = (useLoop && r > 0) ? c.loopStart : 0;
            const int to = (useLoop && r + 1 < reps)
                               ? std::min(keep - 1, c.loopEnd)
                               : keep - 1;
            if (from > to) continue;   // belt-and-braces: never an empty pass
            for (int f = from; f <= to; ++f) {
                p.quats.push_back(c.quats[static_cast<size_t>(f)]);
                if (haveRootY) p.rootY.push_back(c.rootY[static_cast<size_t>(f)]);
                if (haveFingers)
                    p.fingers.push_back(c.fingers[static_cast<size_t>(f)]);
            }
        }
        if (!p.quats.empty()) pieces.push_back(std::move(p));
    }

    if (pieces.empty()) {
        out.error = QStringLiteral("no usable clips for the parsed actions");
        return out;
    }

    const MotionLibrary::Clip& seed = lib.clip(firstTake);
    out.fps = seed.fps;
    out.jointCount = lib.jointCount();
    // Reference triples come from the first take: they describe the SOURCE
    // rig's bind pose, and every library take shares one canonical skeleton, so
    // carrying the first is correct and keeps the retarget on its
    // bind-referenced path rather than the standing-pose fallback.
    out.restDir = seed.restDir;
    out.restWorld = seed.restWorld;
    out.fingerRestDir = seed.fingerRestDir;
    // refRoll is assigned below, gated on singleTake — unlike restDir/restWorld
    // it is NOT a rig-level constant (see the note there).

    // ---- 2. Stitch ---------------------------------------------------------
    const bool wantRootY =
        std::all_of(pieces.begin(), pieces.end(),
                    [](const Piece& p) { return !p.rootY.empty(); });

    out.quats = pieces.front().quats;
    if (wantRootY) out.rootY = pieces.front().rootY;
    out.actions.push_back(pieces.front().action);
    // Fingers survive only a SINGLE-take composition (one action, possibly
    // repeated). Across several takes the seam blend reflows the body while the
    // finger channel cannot follow, so one take's curls would land on another's
    // pose — worse than no fingers at all.
    out.singleTake = pieces.size() == 1;
    if (out.singleTake
        && pieces.front().fingers.size() == pieces.front().quats.size())
        out.fingers = pieces.front().fingers;

    // #1023 review: refRoll survives only a SINGLE-take composition, for the
    // same reason fingers do. restDir/restWorld describe the SOURCE RIG and
    // are shared by every take, so carrying the first is correct; refRoll is
    // the per-clip bind->REFERENCE roll and the reference frame is chosen per
    // extraction, so takes genuinely disagree — measured up to 3.94 rad (226
    // deg) between library clips, and 2.13 rad (122 deg) between run and
    // punch, a pairing a composed prompt can actually produce.
    // applyMotionClip adds refRoll as one per-role constant across EVERY
    // frame, so a multi-take clip would apply the first take's baseline over
    // the others' segments. Correct handling is a PER-FRAME baseline, which
    // needs a new applyMotionClip parameter; until then, emitting nothing is
    // strictly better than emitting a known-wrong constant (empty => the
    // pre-#954 legacy path, which shipped for months).
    if (out.singleTake) out.refRoll = seed.refRoll;

    for (size_t i = 1; i < pieces.size(); ++i) {
        const Piece& nextP = pieces[i];
        const auto [endA, startB] =
            bestCut(out.quats, nextP.quats, std::max(1, blendFrames * 2));

        // Trim the tail we cut away, then blend across the junction so the
        // pose eases from take A into take B instead of snapping.
        out.quats.resize(static_cast<size_t>(endA) + 1);
        if (wantRootY) out.rootY.resize(static_cast<size_t>(endA) + 1);

        const int seam = static_cast<int>(out.quats.size());
        out.seamFrames.push_back(seam);

        const int avail = static_cast<int>(nextP.quats.size()) - startB;
        const int blend = std::clamp(blendFrames, 0, std::max(0, avail - 1));
        for (int k = 0; k < avail; ++k) {
            const size_t src = static_cast<size_t>(startB + k);
            std::vector<std::array<float, 4>> pose = nextP.quats[src];
            if (k < blend && !out.quats.empty()) {
                // t ramps 0→1 across the blend window; at k=0 we are still
                // essentially the last frame of A, so the junction is C0.
                const float t = static_cast<float>(k + 1)
                              / static_cast<float>(blend + 1);
                const auto& prev = out.quats.back();
                for (size_t j = 0; j < pose.size() && j < prev.size(); ++j)
                    pose[j] = slerp(prev[j], pose[j], t);
            }
            out.quats.push_back(std::move(pose));
            if (wantRootY) {
                float y = nextP.rootY[src];
                if (k < blend && !out.rootY.empty()) {
                    const float t = static_cast<float>(k + 1)
                                  / static_cast<float>(blend + 1);
                    y = out.rootY.back() + (y - out.rootY.back()) * t;
                }
                out.rootY.push_back(y);
            }
        }
        out.actions.push_back(nextP.action);
    }

    out.ok = !out.quats.empty();
    if (!out.ok) out.error = QStringLiteral("composition produced no frames");
    return out;
}

// ---------------------------------------------------------------------------
// Surface-facing selection
// ---------------------------------------------------------------------------

MotionComposer::Selection MotionComposer::selectForPrompt(
    const QString& prompt, const MotionLibrary& lib,
    const QByteArray& scriptJson)
{
    Selection sel;
    const Script script = scriptJson.isEmpty() ? parse(prompt, lib)
                                               : parseJson(scriptJson, lib);
    sel.unresolved = script.unresolved;

    // A single step with no repeat/duration is the ORIGINAL single-clip path —
    // take it verbatim rather than routing through compose(), so plain
    // one-action prompts keep their exact shipped behaviour (including the
    // finger side-channel, which a stitched multi-take clip cannot carry
    // coherently). A single step that asks for a REPEAT or a duration still
    // needs the compiler, or "wave twice" silently plays once.
    // The shortcut re-runs matchPrompt(prompt), which searches only the PROMPT
    // text — so it is valid ONLY for a prompt-parsed script. An explicit
    // `script` must be honoured on its own terms: MCP allows a script with no
    // prompt (which matched nothing) or with an unrelated prompt (which
    // silently played the prompt's clip instead of the requested action).
    const bool trivialSingle =
        scriptJson.isEmpty()
        && script.steps.size() == 1 && script.steps.front().repeat <= 1
        && script.steps.front().durationS <= 0.0f;
    if ((scriptJson.isEmpty() && script.steps.empty()) || trivialSingle) {
        QString action;
        const int idx = lib.matchPrompt(prompt, &action);
        if (idx < 0) {
            sel.error = QStringLiteral("no motion matched \"%1\"").arg(prompt);
            return sel;
        }
        const MotionLibrary::Clip& c = lib.clip(idx);
        sel.ok = true;
        sel.action = action;
        sel.quats = c.quats;
        sel.rootY = c.rootY;
        sel.restDir = c.restDir;
        sel.restWorld = c.restWorld;
        sel.refRoll = c.refRoll;
        sel.fingers = c.fingers;
        sel.fingerRestDir = c.fingerRestDir;
        sel.fps = c.fps;
        sel.steps.push_back(action);
        sel.verticalDescent = MotionLibrary::isVerticalDescentAction(action);
        sel.singleTake = true;
        return sel;
    }

    const Composition comp = compose(script, lib);
    if (!comp.ok) {
        sel.error = comp.error;
        return sel;
    }
    sel.ok = true;
    sel.composed = true;
    sel.quats = comp.quats;
    sel.rootY = comp.rootY;
    sel.restDir = comp.restDir;
    sel.restWorld = comp.restWorld;
    sel.refRoll = comp.refRoll;
    sel.fps = comp.fps;
    sel.steps = comp.actions;
    sel.singleTake = comp.singleTake;
    sel.fingers = comp.fingers;
    sel.fingerRestDir = comp.fingerRestDir;
    for (const QString& a : comp.actions)
        if (MotionLibrary::isVerticalDescentAction(a)) {
            sel.verticalDescent = true;
            break;
        }
    // Name the clip after the sequence, e.g. "walk_sit_wave", so repeated
    // generations of different prompts don't collide on one animation name.
    QStringList parts;
    for (const QString& a : comp.actions)
        if (parts.isEmpty() || parts.back() != a) parts << a;
    sel.action = parts.join(QLatin1Char('_'));
    return sel;
}
