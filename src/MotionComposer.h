#ifndef MOTION_COMPOSER_H
#define MOTION_COMPOSER_H

#include "MotionLibrary.h"

#include <QString>
#include <array>
#include <vector>

// COMPOSED text-to-motion (#1010, epic #818 Track A1).
//
// MotionLibrary::matchPrompt answers a whole prompt with ONE clip, so
// "walk forward, sit down, wave twice, then stand up" plays a single walk.
// This composer turns a multi-step prompt into a TIMELINE of library takes and
// stitches them into one continuous canonical clip, which then rides the
// existing AnimationMerger::applyMotionClip retarget with no new rig code.
//
// Three stages, each independently testable:
//   1. parse()    prompt -> MotionScript (ordered steps + repeat/duration)
//   2. compile()  each step -> a concrete take, cut/looped to length
//   3. stitch()   consecutive takes joined at their best-matching pose pair,
//                 blended over a short window so seams don't pop
//
// Ogre-free and unit-testable, like MotionLibrary itself. The LLM parser lives
// in the caller (it needs LLMManager); parse() here is the DETERMINISTIC
// rule-based splitter that must work with no GGUF installed — the epic requires
// the feature never hard-depend on a model download.
class MotionComposer {
public:
    /// One requested action in the timeline.
    struct Step {
        QString action;         ///< resolved library action ("walk", "sit", …)
        QString rawText;        ///< the prompt fragment this came from
        int repeat = 1;         ///< play the take N times back-to-back
        float durationS = 0.0f; ///< 0 = the take's natural length
    };

    /// A parsed prompt: the ordered steps to play.
    struct Script {
        std::vector<Step> steps;
        std::vector<QString> unresolved;  ///< fragments no action matched
        bool empty() const { return steps.empty(); }
    };

    /// The stitched result — shaped exactly like a MotionLibrary::Clip payload
    /// so it feeds applyMotionClip unchanged.
    struct Composition {
        bool ok = false;
        QString error;
        std::vector<std::vector<std::array<float, 4>>> quats;  // [frame][joint]
        std::vector<float> rootY;        ///< per-frame descent, empty when flat
        std::vector<std::array<float, 3>> restDir;   ///< from the first take
        std::vector<std::array<float, 4>> restWorld; ///< from the first take
        /// #1023: per-clip bind->reference roll, carried ONLY for a
        /// single-take composition. Takes disagree (measured up to 226 deg),
        /// and applyMotionClip applies it as one constant across every frame,
        /// so a stitched clip would wear the first take's baseline throughout.
        /// Empty on a multi-take composition (legacy path) until a per-frame
        /// baseline exists.
        std::vector<float> refRoll;
        int fps = 30;
        int jointCount = 22;
        std::vector<QString> actions;    ///< one entry per composed step
        std::vector<int> seamFrames;     ///< frame index of each junction
        /// V1 finger side-channel, carried ONLY when the whole composition came
        /// from one take (see Selection::singleTake).
        std::vector<std::vector<std::array<float, 4>>> fingers;
        std::vector<std::array<float, 3>> fingerRestDir;
        bool singleTake = false;
        int frames() const { return static_cast<int>(quats.size()); }
    };

    /// Split a free-text prompt into ordered steps WITHOUT an LLM.
    ///
    /// Segments on connectives ("then", "and then", "after that", ",", ";",
    /// "finally", "next") and reads a per-segment repeat count ("twice",
    /// "3 times") and duration ("for 2 seconds"). Each segment resolves through
    /// MotionLibrary::resolveAction, so the composer's vocabulary is exactly
    /// the library's — no second synonym table to drift.
    ///
    /// A single-action prompt yields a one-step script, which composes to that
    /// take alone: composing is then equivalent to the old single-clip path.
    static Script parse(const QString& prompt, const MotionLibrary& lib);

    /// #1034: does this prompt describe MORE THAN ONE step?
    ///
    /// Splits on the same connectives `parse` uses ("then", ",", "and", …) but
    /// needs NO MotionLibrary, so a caller can ask before paying for the
    /// library download/parse. Deliberately conservative: it counts segments,
    /// not resolvable actions, so "walk then flibbertigibbet" reports true and
    /// the caller routes to the path that can at least report the unresolved
    /// fragment. A single action with a repeat or duration ("wave twice") is
    /// NOT multi-step — one take covers it.
    ///
    /// Exists because the trained-model path (MotionGenerator) consumes the
    /// whole prompt as one text condition and emits one clip, silently
    /// blending a sequenced prompt into a single averaged pose.
    static bool promptHasMultipleSteps(const QString& prompt);

    /// Parse a MotionScript JSON payload (the shape an LLM or an MCP caller
    /// supplies directly): {"steps":[{"action":"walk","repeat":1,
    /// "duration_s":2.0}, …]}. Actions still resolve through the library, so an
    /// invented action is reported in `unresolved` rather than silently played.
    static Script parseJson(const QByteArray& json, const MotionLibrary& lib);

    /// Compile + stitch a script into one continuous clip.
    /// `blendFrames` is the crossfade half-window at each seam (0 = hard cut).
    static Composition compose(const Script& script, const MotionLibrary& lib,
                               int blendFrames = 6);

    /// One resolved clip payload, whatever produced it. Surfaces (CLI, MCP,
    /// GUI) fill their applyMotionClip arguments from this so the composed and
    /// single-clip paths cannot drift apart.
    struct Selection {
        bool ok = false;
        QString error;
        bool composed = false;          ///< multi-step composition ran
        QString action;                 ///< clip name suffix / reported action
        std::vector<std::vector<std::array<float, 4>>> quats;
        std::vector<float> rootY;
        std::vector<std::array<float, 3>> restDir;
        std::vector<std::array<float, 4>> restWorld;
        std::vector<float> refRoll;
        std::vector<std::vector<std::array<float, 4>>> fingers;
        std::vector<std::array<float, 3>> fingerRestDir;
        int fps = 30;
        std::vector<QString> steps;     ///< composed step actions (for logging)
        /// True when every frame came from ONE library take (a single action,
        /// possibly repeated). Fingers ride along in that case; a clip stitched
        /// from SEVERAL takes drops them, because one take's curl timing would
        /// land on another take's body.
        bool singleTake = false;
        /// True when ANY composed step is a vertical-descent action. The
        /// descent gate is per-CLIP but `rootY` is per-FRAME, and the
        /// composition carries each step's own values (locomotion frames sit
        /// at ~0), so opening the gate lowers only the steps that asked for it.
        /// Without this a composed name like "walk_sit_wave" matches no
        /// canonical label and the sit silently loses its crouch.
        bool verticalDescent = false;
        std::vector<QString> unresolved;///< prompt fragments that matched nothing
    };

    /// Resolve a prompt to a clip payload: COMPOSE when the prompt parses to
    /// more than one step, otherwise fall back to the library's single-clip
    /// match so existing one-action behaviour is bit-identical.
    ///
    /// `scriptJson` (optional) bypasses parsing and supplies the timeline
    /// directly — the MCP `script` parameter.
    static Selection selectForPrompt(const QString& prompt,
                                     const MotionLibrary& lib,
                                     const QByteArray& scriptJson = {});

    // ---- Pure helpers (exposed for unit tests) -----------------------------

    /// Squared canonical-pose distance between two poses: summed quaternion
    /// geodesic over the joints, feet (canonical 17/21) weighted 2x. Used to
    /// choose where to cut between two takes.
    static double poseDistance(const std::vector<std::array<float, 4>>& a,
                               const std::vector<std::array<float, 4>>& b);

    /// Best (endFrame, startFrame) cut pair joining take `a` to take `b`:
    /// searches the last `window` frames of `a` against the first `window` of
    /// `b` and returns the pair minimising poseDistance. Keeps at least one
    /// frame of each take.
    static std::pair<int, int> bestCut(
        const std::vector<std::vector<std::array<float, 4>>>& a,
        const std::vector<std::vector<std::array<float, 4>>>& b,
        int window);

    /// Shortest-arc quaternion slerp (unit in, unit out).
    static std::array<float, 4> slerp(const std::array<float, 4>& a,
                                      const std::array<float, 4>& b, float t);
};

#endif // MOTION_COMPOSER_H
