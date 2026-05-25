#ifndef PSXHMDRAMSCANNER_H
#define PSXHMDRAMSCANNER_H

#include <cstddef>
#include <cstdint>

class EmuHooks;

/**
 * Scan PS1 system RAM for Sony SDK HMD (`0x00000050` magic) hierarchical mesh structures.
 * v1 is a *stub* — HMD's hierarchical primitive-node layout is significantly more involved
 * than TMD's flat object table and parsing it speculatively from RAM is a common source of
 * false-positives. The scaffold exists so per-title testing can be enabled at runtime via
 * `QTMESH_PS1_HMD_SCANNER=1` without needing a code change; future revisions will land the
 * actual primitive walk here once we have a known-good HMD asset to test against (#674).
 */
class PsxHmdRamScanner
{
public:
    /** Returns the number of meshes accepted by `hooks->onModelMesh`. v1 returns 0 unless
     *  `QTMESH_PS1_HMD_SCANNER` env var is set, in which case the magic-only scan runs to
     *  surface candidate addresses via Sentry breadcrumbs (still 0 meshes emitted). */
    static int captureFromSystemRam(const uint8_t *ram, size_t byteSize, EmuHooks *hooks);

    /** Probe for HMD magic bytes only. Used by the v1 stub to count candidates without
     *  emitting meshes. Exposed for unit tests. */
    static int countHmdCandidates(const uint8_t *ram, size_t byteSize);
};

#endif // PSXHMDRAMSCANNER_H
