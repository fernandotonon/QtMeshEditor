/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef VERTEXANIMATIONMANAGER_H
#define VERTEXANIMATIONMANAGER_H

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <cstdint>
#include <vector>

namespace Ogre { class Entity; class Mesh; }

/**
 * @brief Full-mesh (per-vertex) animation — Anim epic Slice B (#519).
 *
 * Where MorphAnimationManager (Slice A) drives a handful of NAMED blend-shape
 * weights, this manager owns clips where EVERY vertex moves per frame with no
 * skeleton — cloth, sims, fluid bakes, destruction, and Alembic caches from
 * Houdini / Blender.
 *
 * Both use Ogre's VertexAnimationTrack; the difference is intent + density: a
 * morph clip has a few poses the user blends, a vertex-anim clip has one dense
 * per-frame shape. We reuse Ogre's VAT_POSE path (one Pose per frame, keyed at
 * full weight at that frame's time) which the viewport/timeline already play —
 * so the timeline scrubber, loop, and dope sheet work with no new playback code.
 *
 * SUB-SLICE LAYERING (see issue #519 / the plan):
 *   - B1 (this file): the manager + the CPU-side "sample buffer -> Ogre
 *     VAT_POSE Animation on the mesh" builder + playback/dope-sheet wiring +
 *     headless tests, all reachable WITHOUT the Alembic dependency (a synthetic
 *     buffer is enough to exercise + test the whole path).
 *   - B2: the real Alembic reader (behind -DENABLE_ALEMBIC) fills a sample
 *     buffer and hands it to `buildClipFromFrames`.
 *   - B3: disk-streaming for caches too large to hold resident, CLI/MCP, and
 *     .abc -> FBX vertex-cache convert.
 *
 * The pure-data core (`FrameData`, `buildClipFromFrames`, `sampleHeuristic`)
 * takes flat float arrays and an Ogre::Mesh, so it is unit-testable under
 * headless CI with a synthetic cube-wobble buffer — no Alembic, no GL.
 */
class VertexAnimationManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    static VertexAnimationManager* instance();
    static VertexAnimationManager* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    /// How a vertex-anim clip is stored on the mesh. The issue's heuristic:
    /// small caches become blend-able poses (cheap, GPU-friendly); large ones
    /// would stream (B3). `sampleHeuristic()` picks per the frame count.
    enum class Storage { Poses, Stream };

    /// One decoded frame's vertex data. Positions are flat xyz triples in the
    /// mesh's own space, `vertexCount` long. Optional per-frame normals (same
    /// layout) improve shading on deforming meshes; empty = recompute/none.
    struct FrameData {
        float time = 0.0f;                 ///< seconds from clip start
        std::vector<float> positions;      ///< 3 * vertexCount
        std::vector<float> normals;        ///< 0 or 3 * vertexCount
    };

    /// Decoded, source-agnostic vertex-animation clip (what an Alembic reader,
    /// or the synthetic generator in tests, produces). Pure data.
    struct FrameSet {
        int vertexCount = 0;
        int fps = 30;
        std::vector<FrameData> frames;     ///< time-ordered, >= 2 to animate
        std::array<float, 6> aabb{ {0,0,0,0,0,0} };  ///< minXYZ, maxXYZ over all frames
        bool ok() const { return vertexCount > 0 && frames.size() >= 2; }
    };

    /// Pure-data heuristic (issue #519): < 32 keyframes -> pose blending,
    /// otherwise a streamed/flat path. Public + static so it is unit-tested.
    static Storage sampleHeuristic(int frameCount);

    /// Build an Ogre VAT_POSE Animation named `clipName` on `mesh` from a
    /// decoded FrameSet: one Pose per frame (delta vs. the mesh's bind
    /// positions) with a VertexAnimationTrack keyed to full weight at each
    /// frame's time. The mesh must already have `frames.vertexCount` vertices
    /// in submesh 0 / shared geometry. Returns false (and adds nothing) on
    /// mismatch. Ogre-only, no GL required — safe under headless CI.
    ///
    /// This is the Storage::Poses path. Storage::Stream (large caches) is B3;
    /// until then a too-large FrameSet still builds poses (correct, just
    /// heavier) so nothing silently fails.
    static bool buildClipFromFrames(Ogre::Mesh* mesh,
                                    const QString& clipName,
                                    const FrameSet& frames);

    /// True when `entity` has at least one VAT_POSE (vertex-anim or morph)
    /// animation. Used by the UI to show the "Mesh" dope-sheet row.
    bool hasVertexAnimation(Ogre::Entity* entity) const;

    /// Names of the vertex-animation clips on `entity` (Ogre animations that
    /// carry a vertex track). Empty when none / null.
    QStringList vertexClipsFor(Ogre::Entity* entity) const;

    /// QML-friendly variants resolving the entity from SelectionSet.
    Q_INVOKABLE QStringList vertexClipsForSelection() const;
    Q_INVOKABLE bool selectionHasVertexAnimation() const;

signals:
    /// Emitted when the vertex-anim clip list visible to the UI could have
    /// changed (import, selection moved, scene reloaded).
    void vertexAnimationsChanged();

private:
    explicit VertexAnimationManager(QObject* parent = nullptr);
    ~VertexAnimationManager() override;

    static VertexAnimationManager* s_instance;
};

#endif // VERTEXANIMATIONMANAGER_H
