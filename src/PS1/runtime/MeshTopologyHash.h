#ifndef MESHTOPOLOGYHASH_H
#define MESHTOPOLOGYHASH_H

#include "MeshReconstructor.h"

#include <QtGlobal>

/** Loose rounds positions to 0.01; strict uses bit-exact floats (#423). */
enum class MeshDedupeMode { Loose, Strict };

/** Topology fingerprint for mesh deduplication (#423). */
class MeshTopologyHash
{
public:
    static quint64 hashMesh(const ReconstructedMesh &mesh, MeshDedupeMode mode);

    /** Subtract mesh centroid; writes centroid to out params. */
    static ReconstructedMesh centered(const ReconstructedMesh &mesh, float &cxOut, float &cyOut, float &czOut);
};

#endif // MESHTOPOLOGYHASH_H
