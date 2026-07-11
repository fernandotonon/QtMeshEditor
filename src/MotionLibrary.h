#ifndef MOTION_LIBRARY_H
#define MOTION_LIBRARY_H

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
        // order (size frames × 22). Rotation only; translation/scale are the
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

    // Reference bone directions (22 × [x,y,z]) for a MODEL-generated clip:
    // model output carries no reference triple, so its base pose is
    // synthesized from a TEMPLATE clip's restDir instead of harvesting the
    // rig's other animations (which contaminated generations with e.g. a
    // dance stance). Prefers a clip matching `prompt`, falls back to any
    // clip carrying restDir. Reads the LOCAL library only (no download);
    // returns empty when unavailable — callers then keep the harvest path.
    static std::vector<std::array<float, 3>> referenceDirsForPrompt(
        const QString& prompt);

private:
    bool parse(const QByteArray& json);
    std::vector<Clip> m_clips;
    std::vector<std::array<float, 4>> m_cmuRestWorld;  // 22 quats, or empty (v1)
    bool m_worldFrame = false;                         // true for schema v3
    QString m_error;
};

#endif // MOTION_LIBRARY_H
