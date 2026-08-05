#ifndef MOTION_LIBRARY_H
#define MOTION_LIBRARY_H

#include <QSet>
#include <QString>
#include <array>
#include <vector>

// Text-to-motion TEMPLATE-CLIP library (#411 MVP, epic #397).
//
// The #411 spike showed a from-scratch GENERATIVE text-to-motion model collapses
// to a static pose without multi-day ML effort (see
// docs/TEXT_TO_MOTION_SPIKE_411.md). The shipped MVP is instead a curated library
// of real, permissively-licensed motion clips (CMU MoCap, commercial-OK — the
// same source as the #409 RMIB model), matched to a prompt by ACTION KEYWORD and
// retargeted onto the user's skeleton via the #409 canonical-joint mapping
// (MotionInbetween::canonicalIndexForBone).
//
// This core is Ogre-free + unit-testable: it loads/parses the library JSON and
// does prompt→clip matching on flat per-frame, per-joint quaternion arrays.
// AnimationMerger adapts a matched clip onto an Ogre::Skeleton.
//
// The library JSON is downloaded on first use to AppData/ai_models/motion/ (the
// #404/#408/#409/#410 per-user-cache convention), built offline by
// scripts/build-motion-library.py. Joint order MUST match the 22 canonical CMU
// core-body joints (MotionInbetween::canonicalJointName).
class MotionLibrary {
public:
    // One clip: a per-frame, per-joint canonical-pose sequence.
    struct Clip {
        QString action;                 // "walk", "run", "jump", …
        QString source;                 // provenance, e.g. "CMU 02_01"
        int frames = 0;
        int fps = 30;
        // quats[frame][joint] = (x,y,z,w) unit quaternion, joint in canonical
        // order (size frames × jointCount(): 22 for schema v1..v3, 52 for v4
        // with fingers folded in). Rotation only; translation/scale are the
        // caller's (the retarget writes rotation keyframes).
        std::vector<std::vector<std::array<float, 4>>> quats;
        // Optional (schema v3 clips extracted by --dump-canonical): the
        // SOURCE rig's bind-pose world orientation per canonical joint.
        // Present → the retarget runs bind-referenced (deltas vs the source
        // bind onto the target bind); absent (CMU-built libraries) → the
        // standing-pose path.
        std::vector<std::array<float, 4>> restWorld;
        // Optional: canonical-frame bind bone directions (22 × [x,y,z]) —
        // enables the direction-aligned bind-referenced retarget.
        std::vector<std::array<float, 3>> restDir;
        // Optional (#838 vertical descent): per-frame crouch DEPTH in
        // LEG-LENGTHS (≤ 0), preserving BIND-POSE-relative hip height — a clip
        // that opens already crouched keeps its lowered value (NOT re-based to
        // the window's first frame), so always-low actions like crawl stay
        // down. Measured as the hip's height above the foot minus the rig's
        // bind-pose standing height, normalized by the source hip→foot
        // distance; size == frames. The retarget scales it by the TARGET rig's
        // leg length and lowers the root bone's Y (descent-only) so crouch/
        // pickup/working actually sink. Empty → flat root (locomotion clips).
        std::vector<float> rootY;
        /// Optional (#838 finger animation): per-frame LOCAL finger curl,
        /// size frames × 30 (2 sides × 5 fingers × 3 segments, see
        /// AnimationMerger::fingerSlot). Empty when the source rig has no
        /// fingers. The retarget maps these onto the target rig's fingers.
        std::vector<std::vector<std::array<float, 4>>> fingers;
        /// Optional (#838): SOURCE finger REST pointing directions, size 30
        /// (kFingerSlots × [x,y,z], canonical frame). Present → the retarget
        /// transports RELATIVE finger bend (rest→frame) so a rig's finger rest
        /// convention doesn't over-bend the target; absent → legacy absolute
        /// aim (older libraries).
        std::vector<std::array<float, 3>> fingerRestDir;
        /// Curation score 0..1 from the library builder (#855) — take
        /// selection samples proportionally to quality². Absent → 1.0.
        float quality = 1.0f;
    };

    MotionLibrary() = default;

    // ---- Loading -----------------------------------------------------------
    // Parse a library JSON (schema "qtmesh-motion-library-v1") from a file or a
    // raw byte buffer. Returns false (and sets error()) on malformed input.
    bool loadFromFile(const QString& path);
    bool loadFromJson(const QByteArray& json);
    bool isLoaded() const { return !m_clips.empty(); }
    QString error() const { return m_error; }

    int clipCount() const { return static_cast<int>(m_clips.size()); }
    const Clip& clip(int i) const { return m_clips.at(i); }

    // CMU per-canonical-joint WORLD-REST orientation (x,y,z,w), 22 entries, or
    // empty when the library is schema-v1 (no rest data). The retarget uses
    // these to build the exact CMU↔target change-of-basis that cancels the
    // per-bone roll twist between CMU's rest axes and the target rig's.
    const std::vector<std::array<float, 4>>& cmuRestWorld() const { return m_cmuRestWorld; }
    bool hasCmuRest() const { return m_cmuRestWorld.size() == 22; }

    // True when clip quats are WORLD-space joint orientations (schema v3,
    // "frame":"world") rather than LOCAL parent-relative rotations (v1/v2).
    // World-space is basis-independent so the retarget delta carries the true
    // per-bone roll (no arm-twist); the adapter branches on this.
    bool isWorldFrame() const { return m_worldFrame; }
    // Joints per pose in this library: 22 (schema v1..v3, body only) or 52
    // (schema v4, fingers folded in as joints 22..51). The retarget uses this to
    // decide whether fingers ride the joint path (v4) or the side-channel (≤v3).
    int jointCount() const { return m_jointCount; }
    // The actions this library can produce (for help text / GUI listing).
    std::vector<QString> actions() const;

    // ---- Prompt matching ---------------------------------------------------
    // Pick the clip whose action best matches a free-text prompt. Matching is
    // keyword-based: the action whose name appears in the prompt wins; ties and
    // synonyms (jog→run, idle/stand→idle, …) are resolved by a small synonym
    // map. Returns the clip index, or -1 if nothing matches. `matchedAction`
    // (optional) reports which action was chosen.
    int matchPrompt(const QString& prompt, QString* matchedAction = nullptr) const;

    // ---- Download / cache (mirrors the ONNX-model pattern) -----------------
    // Absolute path the library is cached at (AppData/ai_models/motion/...).
    static QString libraryPath();
    static bool libraryPresent();
    // Download on first use (blocks via a local ModelDownloader event loop).
    // Returns the path, or empty when offline/disabled/failed. Honours
    // QTMESH_MOTION_NO_DOWNLOAD + base-URL override QTMESH_MOTION_LIBRARY_BASE_URL
    // / QSettings ai/motionLibraryBaseUrl. Call on a thread with an event loop.
    static QString ensureLibraryBlocking();

    // ---- Curation (user-approved "good" clips) ------------------------------
    // The user reviews retargeted clips in the Animation Library picker and
    // marks the good ones; approvals persist as a set of clip `source` strings
    // (stable across library rebuilds) in curation.json next to the library.
    // The library builder consumes the same file (--curation/--approved-only)
    // to SHIP only the approved set while the rest is iterated on.
    static QString curationPath();                    // .../motion/curation.json
    static QSet<QString> loadCuration();              // approved clip sources
    static bool saveCuration(const QSet<QString>& approved);

    // Reference bone directions (22 × [x,y,z]) for a MODEL-generated clip:
    // model output carries no reference triple, so its base pose is
    // synthesized from a TEMPLATE clip's restDir instead of harvesting the
    // rig's other animations (which contaminated generations with e.g. a
    // dance stance). Prefers a clip matching `prompt`, falls back to any
    // clip carrying restDir. Reads the LOCAL library only (no download);
    // returns empty when unavailable — callers then keep the harvest path.
    static std::vector<std::array<float, 3>> referenceDirsForPrompt(
        const QString& prompt);

    // #838 vertical descent: true for NON-LOCOMOTION actions whose motion
    // lowers the body (pickup / working / sit / crawl / death / pray). For
    // these the retarget applies the clip's `rootY` so the body actually
    // crouches; locomotion (walk/run/jump/…) keeps a flat root (returns false).
    static bool isVerticalDescentAction(const QString& action);

private:
    bool parse(const QByteArray& json);
    std::vector<Clip> m_clips;
    std::vector<std::array<float, 4>> m_cmuRestWorld;  // 22 quats, or empty (v1)
    bool m_worldFrame = false;                         // true for schema v3/v4
    int m_jointCount = 22;                              // 22 (v1..v3) or 52 (v4)
    QString m_error;
};

#endif // MOTION_LIBRARY_H
