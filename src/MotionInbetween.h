#ifndef MOTION_INBETWEEN_H
#define MOTION_INBETWEEN_H

#include <QString>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

// AI animation in-betweening (issue #409, epic #397) — fills the gap between two
// sparse keyframes with smooth, plausible intermediate poses.
//
// The issue proposes **Robust Motion In-betweening** (Harvey et al., Ubisoft,
// SIGGRAPH 2020): a small transformer that, given a start pose, an end (target)
// pose and a target duration, predicts the in-between frames. Like the other
// AI-assist features in this epic (#404 PbrMapSynth, #408 UniRig) the ML path
// runs on ONNX Runtime and is **OFF unless built with `ENABLE_ONNX`** plus a
// first-use model download.
//
// **Fallback is first-class, not an afterthought.** RMIB is trained on one
// specific skeleton/feature layout, so it only applies cleanly to humanoid rigs
// close to its training distribution. Per the issue's acceptance criteria, this
// module ALWAYS provides a deterministic **spline fallback** (cubic-Hermite for
// translation/scale, shortest-arc slerp for rotation) that is visibly smoother
// than naive linear interpolation, and is used automatically when:
//   * the binary was built without ENABLE_ONNX,
//   * the model isn't present / can't be downloaded,
//   * the skeleton is incompatible with the model (wrong bone count / layout), or
//   * the model run fails for any reason.
// The caller is told which path produced the result (`Result::usedModel` +
// `fallbackReason`) so the GUI/CLI can surface a "fell back to spline" note.
//
// This core is **Ogre-free and unit-testable**: it works purely on flat pose
// arrays (one std::array<float> per frame, channels = bones × per-bone DoF) and
// has no dependency on Ogre::Animation. `AnimationMerger::inbetweenAnimation`
// adapts it to Ogre node tracks; the CLI / MCP / GUI all go through that.
class MotionInbetween {
public:
    // A single pose, flattened: `channels` floats. The channel layout is the
    // caller's contract — AnimationMerger packs per-bone [tx,ty,tz, qx,qy,qz,qw,
    // sx,sy,sz] (10 DoF/bone) in a stable bone order. The fallback is
    // layout-agnostic (it interpolates each channel independently, treating any
    // 4-tuple flagged as a quaternion with slerp); the ONNX path requires the
    // model's exact layout and otherwise falls back.
    using Pose = std::vector<float>;

    // Which per-channel interpolation a channel uses in the FALLBACK path.
    //   Scalar     — cubic-Hermite (Catmull-Rom tangents) on the raw value.
    //   QuatStart  — first component of a 4-wide quaternion block; the whole
    //                block is slerped as a unit (the other three are QuatCont).
    //   QuatCont   — a continuation component of the quaternion block above;
    //                written by the slerp of its QuatStart, not interpolated
    //                independently.
    enum class Channel : uint8_t { Scalar, QuatStart, QuatCont };

    struct Options {
        Options();
        // Number of intermediate frames to synthesise BETWEEN start and end
        // (exclusive of the two endpoints). gapFrames=N → N new poses.
        int gapFrames = 8;
        // Up axis (0=X,1=Y,2=Z). Forwarded to the ONNX path (RMIB is +Y-up).
        int upAxis = 1;
        // Force the deterministic spline fallback even when a model is present
        // (used by tests and the CLI `--no-model` escape hatch).
        bool forceFallback = false;
    };

    struct Result {
        bool ok = false;
        QString error;                       // populated when !ok
        // The synthesised in-between poses, gapFrames of them, in order from
        // just-after-start to just-before-end.
        std::vector<Pose> frames;
        // True when the ONNX RMIB model produced the frames; false when the
        // spline fallback did. Always check this for telemetry / UX.
        bool usedModel = false;
        // Non-empty when a model run was requested but the fallback ran instead
        // (disabled build, missing model, incompatible skeleton, run failure).
        QString fallbackReason;
    };

    // True only when the binary was compiled with ENABLE_ONNX. (Model presence
    // is a separate, per-call check against the model path.)
    static bool isModelBackendAvailable();

    // ---- Deterministic spline fallback (always available, no ONNX/Ogre) -----
    // Synthesise `opts.gapFrames` intermediate poses between `start` and `end`,
    // given the per-channel layout. Translation/scale channels use cubic-Hermite
    // with Catmull-Rom tangents derived from the optional `preStart`/`postEnd`
    // neighbour poses (so the segment blends smoothly into the surrounding
    // motion); when a neighbour is absent the tangent falls back to the secant,
    // which still beats linear by being C1-continuous at the endpoints.
    // Quaternion blocks are shortest-arc slerped. Never fails for well-formed
    // input (matching channel counts); returns ok=false only on a layout
    // mismatch.
    static Result interpolateSpline(const Pose& start, const Pose& end,
                                    const std::vector<Channel>& layout,
                                    const Options& opts,
                                    const Pose* preStart = nullptr,
                                    const Pose* postEnd = nullptr);

    // ---- Canonical skeleton (RMIB retargeting) ------------------------------
    // The hosted RMIB model is trained on a FIXED 22-joint humanoid skeleton
    // (CMU core body: hips/spine/neck/head + both arms + both legs), so its
    // input/output channel count is canonicalJointCount()*10 = 220. Real user
    // rigs have different bone counts/names, so to run the model on them the
    // adapter maps the rig's bones onto these 22 canonical roles BY NAME (Mixamo
    // `mixamorig:LeftArm`, generic `L_Shoulder`, CMU `lshoulder`, …). Bones that
    // don't map keep the spline; if too few roles resolve, the whole segment
    // falls back. This is what makes the skeleton-specific model usable on
    // arbitrary humanoid rigs instead of always-fallback.
    static int canonicalJointCount();              // 22
    // Canonical joint name at index i (CMU core-body name), 0..count-1.
    static QString canonicalJointName(int i);
    /// Canonical skeleton topology (22 joints): parent index (-1 for hip)
    /// and the primary child used for bone-direction computation (-1 for
    /// leaf joints: head, hands, feet).
    static int canonicalParentOf(int i);
    static int canonicalChildOf(int i);
    // Map an arbitrary skeleton bone name to a canonical joint index, or -1 if
    // it doesn't correspond to one of the 22 roles. Case-insensitive; tolerates
    // common prefixes (mixamorig:, mixamorig1:, bip01 …) and side spellings
    // (left/right, l/r, _l/_r). Pure-data + unit-testable.
    static int canonicalIndexForBone(const QString& boneName);

    // ---- ONNX RMIB path (model when available, else spline) -----------------
    // Absolute path the RMIB model is expected at
    // (AppData/ai_models/inbetween/rmib.onnx). Same per-user cache convention as
    // the #404/#408 models.
    static QString modelPath();
    // True when the model file already exists on disk (no download needed).
    static bool modelPresent();

    // Ensure the RMIB model exists on disk, downloading it on first use (blocks
    // via a local event loop on ModelDownloader, like
    // UniRigPredictor::ensureModelBlocking). Returns the model path on success,
    // or empty when offline / disabled / the download failed — in which case the
    // caller simply uses the spline fallback. Honours QTMESH_INBETWEEN_NO_DOWNLOAD
    // (tests/offline) and the base-URL override QTMESH_INBETWEEN_MODEL_BASE_URL /
    // QSettings ai/inbetweenModelBaseUrl. MUST be called on a thread with an
    // event loop (the GUI/CLI main thread).
    static QString ensureModelBlocking();

    // Run RMIB if a usable model is present at `modelPath`; otherwise (disabled
    // build / missing model / incompatible layout / run failure) fall back to
    // interpolateSpline and record why in `fallbackReason`. `progress` (optional)
    // is reserved for parity with the other predictors; RMIB is a single fast
    // forward pass so it's called once. Never throws.
    static Result predict(const Pose& start, const Pose& end,
                          const std::vector<Channel>& layout,
                          const QString& modelPath,
                          const Options& opts = {},
                          const Pose* preStart = nullptr,
                          const Pose* postEnd = nullptr);

    // ---- Pure-data helpers (no ONNX / no Ogre — unit-testable) --------------
    // Shortest-arc slerp of two quaternions packed as [x,y,z,w]. Exposed for the
    // adapter + tests. `t` in [0,1].
    static std::array<float, 4> slerpQuat(const std::array<float, 4>& a,
                                          const std::array<float, 4>& b,
                                          float t);
    // Cubic-Hermite scalar interpolation with the given endpoint tangents.
    static float hermite(float p0, float p1, float m0, float m1, float t);
};

#endif // MOTION_INBETWEEN_H
