#ifndef PS1ANIMATIONEXTRACTOR_H
#define PS1ANIMATIONEXTRACTOR_H

#include "CaptureTypes.h"

#include <QVector>

#include <cstdint>

/**
 * Rigid animation extraction from per-frame in-core GTE records (#429).
 *
 * The premise (from the issue's rewrite): with #814 every frame of a
 * *Capture Scene* delivers per-vertex `qtmesh_rip_gte_record`s carrying the
 * object-space vertex PLUS the exact `(rt, tr)` matrix each object was drawn
 * with, each frame. For the common PS1 case — rigid / hierarchical motion —
 * an object's object-space vertices are IDENTICAL every frame and only its
 * `(rt, tr)` changes. So animation capture reduces to: group records by
 * object identity, then record one matrix key per frame. No vertex diffing,
 * no correspondence problem.
 *
 * This is the Ogre-free, unit-tested core. It takes the accumulated scene
 * `gteRecords` and produces one `MatrixTrack` per object that appears in
 * >= `minFrames` distinct frames with a *changing* matrix. The Ogre adapter
 * (Ps1AnimationBuilder / PS1RipMeshBuilder) turns each track into an
 * `Ogre::NodeAnimationTrack` on the object's capture node.
 *
 * Object identity: all records emitted for one object in one frame share the
 * same rotation+translation. Across frames the matrix changes but the
 * OBJECT-SPACE vertex set is stable, so we key an object by a hash of its
 * sorted object-space vertices. (Two genuinely distinct objects that happen to
 * share an identical vertex set — e.g. instanced props — collapse to one
 * track; that's acceptable for v1 and matches the #816 dedupe behaviour.)
 *
 * Caveat carried from #816: GTE matrices are usually `View × Model`, so tracks
 * include camera motion. A static-camera scene is the v1 target (pause-screen /
 * idle-animation captures) — documented, not enforced.
 */

/** One rigid pose sample for an object at a given capture frame. */
struct Ps1MatrixKey {
    uint32_t frame = 0;   /* core frame counter */
    int32_t rt[9] = {4096, 0, 0, 0, 4096, 0, 0, 0, 4096}; /* 4.12 fixed (identity) */
    int32_t tr[3] = {0, 0, 0};                            /* GTE translation */
};

/** One animated object: its stable identity + per-frame matrix samples,
 *  ordered by frame ascending. */
struct Ps1MatrixTrack {
    quint64 objectKey = 0;          /* object-space vertex-set hash */
    QVector<Ps1MatrixKey> keys;     /* frame-ordered, deduped-by-frame */
};

class Ps1AnimationExtractor {
public:
    struct Options {
        /** An object must appear in at least this many distinct frames to be a
         *  track candidate (below this it's a static prop, not animation). */
        int minFrames = 2;
        /** Skip objects whose matrix never changes across the captured frames
         *  (static geometry — no track worth authoring). */
        bool requireMotion = true;
    };

    /** Extract rigid matrix tracks from accumulated scene records. Returns one
     *  track per qualifying object, each with frame-ordered keys. Empty when
     *  there are no records or nothing qualifies. Deterministic. */
    static QVector<Ps1MatrixTrack> extract(const QVector<GteRecordEntry> &records,
                                           const Options &opts);
    /** Convenience overload using default Options. Defined in the .cpp (not
     *  inline) so there's exactly one emitted definition and no dependence on
     *  the nested struct's initializers at a header default-argument site. */
    static QVector<Ps1MatrixTrack> extract(const QVector<GteRecordEntry> &records);

    /** True when the two matrices differ beyond a small fixed-point epsilon.
     *  Exposed for testing. */
    static bool matricesDiffer(const int32_t aRt[9], const int32_t aTr[3],
                               const int32_t bRt[9], const int32_t bTr[3]);
};

#endif // PS1ANIMATIONEXTRACTOR_H
