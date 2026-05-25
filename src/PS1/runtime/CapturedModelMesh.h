#ifndef CAPTUREDMODELMESH_H
#define CAPTUREDMODELMESH_H

#include "ReconstructedMesh.h"

#include <QString>

#include <cstdint>

/**
 * One mesh ripped from RAM in *model space* (no GTE inverse needed) via a format-aware scanner
 * (TMD/HMD/PMD/HMD/...). The reconstruction pipeline appends these as additional parts alongside
 * the screen-space GP0 prim path; on TMD-using games this is what turns "blob of triangles" into
 * recognizable geometry (#674).
 */
struct CapturedModelMesh {
    /** Mesh data already in editor world units. */
    ReconstructedMesh mesh;
    /** RAM byte offset where the scanner found the source structure (informational/debug). */
    uint32_t sourceAddress = 0;
    /** Short label for the producing format: "tmd", "hmd", ... */
    QString format;
    /** Content hash used for cross-frame dedupe and instance-grouping. */
    uint64_t contentHash = 0;
};

#endif // CAPTUREDMODELMESH_H
