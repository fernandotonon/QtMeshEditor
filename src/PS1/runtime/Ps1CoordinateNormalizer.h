#ifndef PS1COORDINATENORMALIZER_H
#define PS1COORDINATENORMALIZER_H

#include <QString>

class QSettings;
namespace Ogre { class SceneNode; }

/** User-controlled coordinate normalization knobs for PS1 captures (#424).
 *
 *  The reconstruction pipeline emits meshes in editor world space using
 *  source-specific conversions:
 *    - `modelToEditor`         (×0.01, Y-up flip + Z negate)        — GTE-inverted screen-space
 *    - `psxScreenToWorld`      (×0.01, Y-up flip)                    — fallback when sz=0
 *    - `PsxTmdRamScanner`      (×10/4096, 180° Z rotation, CCW swap) — model-space TMD
 *
 *  Those built-in transforms put captures into the right-handed Y-up basis the
 *  editor expects, with face winding matching Ogre's CCW front-face convention.
 *  This struct adds a **uniform multiplier + per-axis sign override** the user
 *  can tweak from the session window without re-capturing, plus an opt-in
 *  perspective-correct UV subdivision that the reconstructor consumes
 *  (`MeshReconstructor::emitPrimitive`).
 *
 *  Per-axis flip is applied as a SceneNode scale of ±1. Ogre flips back-face
 *  culling automatically when the transform determinant is negative, so an
 *  odd number of negated axes still renders the visible side correctly without
 *  any explicit winding swap in the mesh data.
 */
struct Ps1NormalizerSettings {
    /** Multiplicative scale layered on top of the source-specific built-in
     *  conversion (e.g. modelToEditor's 0.01). 1.0 = no extra scaling.
     *  Push toward 0.024 for raw PSX 12.4-native magnitude (1/4096 / 0.01),
     *  or bump higher for games that author small coord ranges.
     *
     *  The issue's stated "default 1/4096" treats the slider as the *absolute*
     *  PSX-native fixed-point divisor; we keep the existing pipeline default
     *  (0.01) so freshly-loaded captures still land in the same magnitude as
     *  FBX/glTF imports — the acceptance criterion. The slider therefore
     *  defaults to 1.0 (no override) and the breadcrumb logs both the user
     *  multiplier and effective magnitude. */
    float userScale = 1.0f;

    bool flipX = false;
    bool flipY = false;
    bool flipZ = false;

    /** When true, MeshReconstructor subdivides triangles whose vertex-depth
     *  ratio exceeds `perspectiveTolerance`, recursing up to
     *  `perspectiveMaxDepth` levels. New midpoint UVs are computed via
     *  screen-space linear interpolation (the PS1 affine convention) so
     *  Ogre's perspective-correct rendering reproduces the artist's intent
     *  at fine grain — the "warped quad fix" used by modern PSX remasters. */
    bool perspectiveCorrectUVs = false;

    /** Subdivide a triangle when max(sz_i) / min(sz_i) > tolerance. 1.0 forces
     *  recursion all the way to max depth; large values (≥ 100) disable it.
     *  Default 1.3 keeps the common case (near-camera quads) untouched. */
    float perspectiveTolerance = 1.3f;

    /** Recursion cap. 0 = no subdivision (perspectiveCorrectUVs is a no-op),
     *  3 = up to 4^3 = 64 sub-tris per input prim. */
    int perspectiveMaxDepth = 3;

    /** "Clean up" filter (#816 follow-up): when true, MeshReconstructor keeps
     *  only vertices placed by the in-core GTE path — Tier 0 (GteTracked) and
     *  Tier 1 (DepthOnly) — and drops every triangle that references a Tier 2
     *  screen-space-fallback vertex. Those Tier-2 prims are the HUD text,
     *  camera-facing sprites, particles and 2D overlays that get dumped into
     *  the same frame's draw list; excluding them leaves the real world
     *  geometry and turns the overlapping "spiky blob" into recognizable
     *  models. No effect on RAM-scan (all-None) captures, which would filter
     *  to empty — guarded at the call site. */
    bool trackedGeometryOnly = false;

    /** Degenerate-triangle cull (#816 spike follow-up): drop any reconstructed
     *  triangle whose longest edge exceeds `spikeEdgeFactor` × the part's
     *  median edge length. A spanning "spike" triangle (a corner that landed in
     *  the wrong model space) has one enormously long edge, so this removes the
     *  visible artifact directly, regardless of its root cause — complementary
     *  to the per-part radius outlier policy. 0 or negative disables it.
     *  Default 12.0: loose enough to keep legitimately elongated PS1 geometry
     *  (thin platforms, long walls) while catching the runaway spans. */
    float spikeEdgeFactor = 12.0f;

    bool isDefault() const;
    bool flipsAreActive() const { return flipX || flipY || flipZ; }

    float signX() const { return flipX ? -1.0f : 1.0f; }
    float signY() const { return flipY ? -1.0f : 1.0f; }
    float signZ() const { return flipZ ? -1.0f : 1.0f; }
};

class Ps1CoordinateNormalizer
{
public:
    /** Pure-data compose helper exposed so tests + non-Ogre callers can verify
     *  the position/scale math without instantiating a SceneNode. Returns the
     *  same transform `applyToSceneNode` writes for the given inputs.
     *
     *  `out*` arrays receive `(x, y, z)` in editor units. `basePos` is the raw
     *  capture-time position (`inst.px/py/pz`, no scaling); `placementScale`
     *  is the auto-fit-to-target-extent factor stamped at attach time. */
    static void composeNodeTransform(const Ps1NormalizerSettings &settings,
                                     float placementScale,
                                     float basePosX, float basePosY, float basePosZ,
                                     float outScale[3], float outPosition[3]);

    /** Reapply `settings` to `node`. Updates both scale AND position so
     *  multi-instance deduped capture layouts scale/mirror as one assembly —
     *  scaling only the per-mesh transform while keeping pivots fixed would
     *  let buildings drift apart at 0.5× or collapse / cross over each other
     *  on a single-axis flip (Codex P1 / CodeRabbit Major). Idempotent —
     *  call again whenever settings change. No-op when `node` is null. */
    static void applyToSceneNode(Ogre::SceneNode *node, const Ps1NormalizerSettings &settings);

    /** Walk every `PS1Capture_*` SceneNode in the editor scene and re-apply
     *  `settings`. Returns the count of nodes touched. Use this after the user
     *  tweaks scale/flip in the UI so the change shows up instantly — no
     *  re-capture, no mesh rebuild. */
    static int applyToCaptureNodes(const Ps1NormalizerSettings &settings);

    /** Persist all settings fields under `prefix` (e.g. "ps1rip/normalize")
     *  so each subkey is independent (`prefix/userScale`, `prefix/flipX`, ...). */
    static void save(QSettings &settings, const QString &prefix,
                     const Ps1NormalizerSettings &value);
    static Ps1NormalizerSettings load(QSettings &settings, const QString &prefix);

    /** Compact one-line descriptor for Sentry breadcrumbs / debug logs.
     *  Returns "default" when no field deviates from `Ps1NormalizerSettings{}`. */
    static QString describe(const Ps1NormalizerSettings &settings);
};

#endif // PS1COORDINATENORMALIZER_H
