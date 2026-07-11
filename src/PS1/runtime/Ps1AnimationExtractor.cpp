#include "Ps1AnimationExtractor.h"

#include <QHash>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace {

// Fixed-point epsilon: GTE rt is 4.12 (4096 == 1.0), tr is integer world units.
// A rotation entry differing by >= 8 (~0.002) or a translation differing by
// >= 1 unit counts as motion. Small enough to catch a slow pan, large enough
// to ignore fixed-point jitter on a genuinely static object.
constexpr int32_t kRtEpsilon = 8;
constexpr int32_t kTrEpsilon = 1;

quint64 mix(quint64 h, quint64 v)
{
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h;
}

} // namespace

QVector<Ps1MatrixTrack> Ps1AnimationExtractor::extract(const QVector<GteRecordEntry> &records)
{
    return extract(records, Options{});
}

bool Ps1AnimationExtractor::matricesDiffer(const int32_t aRt[9], const int32_t aTr[3],
                                           const int32_t bRt[9], const int32_t bTr[3])
{
    for (int i = 0; i < 9; ++i)
        if (std::abs(aRt[i] - bRt[i]) >= kRtEpsilon)
            return true;
    for (int i = 0; i < 3; ++i)
        if (std::abs(aTr[i] - bTr[i]) >= kTrEpsilon)
            return true;
    return false;
}

QVector<Ps1MatrixTrack> Ps1AnimationExtractor::extract(const QVector<GteRecordEntry> &records,
                                                       const Options &opts)
{
    if (records.isEmpty())
        return {};

    // A per-frame matrix cluster: the exact (rt,tr) plus the accumulating hash
    // of the object-space vertices drawn under it that frame.
    struct Cluster {
        int32_t rt[9];
        int32_t tr[3];
        quint64 vertHash = 0;
        int vertCount = 0;
    };

    // objectKey -> (frame -> matrix). We keep the FIRST matrix seen for an
    // (object, frame) pair (all records of one object in one frame share it).
    struct ObjectFrames {
        // frame -> matrix key; insertion via QHash then sorted at the end.
        QHash<uint32_t, Ps1MatrixKey> byFrame;
    };
    QHash<quint64, ObjectFrames> objects;

    // Segment the record stream into per-object DRAW RUNS: a maximal run of
    // consecutive records (in capture order) that share the same frame AND the
    // same matrix. Each PS1 object is transformed and drawn as one contiguous
    // burst of RTPS/RTPT ops, so a seq-contiguous same-matrix run is exactly
    // one object's draw that frame. Clustering purely by matrix (ignoring
    // contiguity) would wrongly merge two DIFFERENT objects that momentarily
    // share a matrix (e.g. both at the world origin on frame 0) — the source
    // of the earlier over/under-count. Contiguity keeps them separate.
    auto matrixSame = [](const GteRecordEntry &a, const GteRecordEntry &b) {
        return !matricesDiffer(a.rt, a.tr, b.rt, b.tr) && a.frame == b.frame;
    };

    int i = 0;
    const int n = records.size();
    while (i < n) {
        // Extend the run while the matrix + frame stay put.
        int j = i + 1;
        while (j < n && matrixSame(records[i], records[j]))
            ++j;

        Cluster c;
        for (int k = 0; k < 9; ++k) c.rt[k] = records[i].rt[k];
        for (int k = 0; k < 3; ++k) c.tr[k] = records[i].tr[k];
        c.vertHash = 1469598103934665603ULL;
        for (int r = i; r < j; ++r) {
            // Order-independent fold so a run's vertex ordering doesn't affect
            // the key (the same object may emit verts in a different order in
            // another frame).
            quint64 vh = static_cast<quint64>(static_cast<uint16_t>(records[r].vx));
            vh = (vh << 16) | static_cast<uint16_t>(records[r].vy);
            vh = (vh << 16) | static_cast<uint16_t>(records[r].vz);
            c.vertHash += (vh * 0x100000001B3ULL);
            ++c.vertCount;
        }

        const uint32_t frame = records[i].frame;
        const quint64 objectKey = mix(c.vertHash, static_cast<quint64>(c.vertCount));
        Ps1MatrixKey key;
        key.frame = frame;
        for (int k = 0; k < 9; ++k) key.rt[k] = c.rt[k];
        for (int k = 0; k < 3; ++k) key.tr[k] = c.tr[k];
        // First draw wins for a given (object, frame) — an object drawn twice
        // in one frame (rare) keeps its first pose.
        ObjectFrames &of = objects[objectKey];
        if (!of.byFrame.contains(frame))
            of.byFrame.insert(frame, key);

        i = j;
    }

    // Build tracks: frame-ordered keys, filtered by minFrames + requireMotion.
    QVector<Ps1MatrixTrack> tracks;
    for (auto oit = objects.constBegin(); oit != objects.constEnd(); ++oit) {
        const ObjectFrames &of = oit.value();
        if (of.byFrame.size() < std::max(1, opts.minFrames))
            continue;

        Ps1MatrixTrack track;
        track.objectKey = oit.key();
        track.keys.reserve(of.byFrame.size());
        for (auto kit = of.byFrame.constBegin(); kit != of.byFrame.constEnd(); ++kit)
            track.keys.append(kit.value());
        std::sort(track.keys.begin(), track.keys.end(),
                  [](const Ps1MatrixKey &a, const Ps1MatrixKey &b) { return a.frame < b.frame; });

        if (opts.requireMotion) {
            bool moved = false;
            const Ps1MatrixKey &first = track.keys.first();
            for (int i = 1; i < track.keys.size() && !moved; ++i)
                moved = matricesDiffer(first.rt, first.tr, track.keys[i].rt, track.keys[i].tr);
            if (!moved)
                continue; // static object — no track worth authoring
        }
        tracks.append(std::move(track));
    }

    // Deterministic order (objectKey) so callers / tests see stable output.
    std::sort(tracks.begin(), tracks.end(),
              [](const Ps1MatrixTrack &a, const Ps1MatrixTrack &b) { return a.objectKey < b.objectKey; });
    return tracks;
}
