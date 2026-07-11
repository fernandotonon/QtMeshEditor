/* qtmesh_rip_abi.h — QtMeshEditor PS1 rip-capture C ABI (issues #813/#814/#815).
 *
 * This header is the contract between the rip-instrumented beetle-psx fork
 * (fernandotonon/beetle-psx-libretro, branch qtmesh-rip) and the QtMeshEditor
 * host (src/PS1/runtime/libretro/qtmesh_rip_abi.h). The two copies MUST stay
 * byte-identical.
 *
 * ABI rules:
 *  - Structs are fixed-layout. Extend only by APPENDING fields; any layout
 *    change bumps QTMESH_RIP_ABI_VERSION.
 *  - The host must refuse to register on a version mismatch.
 *  - The core must behave 100% stock when no interface is registered.
 *  - All callbacks fire synchronously inside retro_run() on the calling
 *    thread; the host must not block.
 *  - Delivery order per frame: zero or more on_gp0_draw callbacks during the
 *    frame, then one on_gte_records flush with every GTE record captured this
 *    frame, then on_frame_end. A draw's vertex shadows may therefore reference
 *    GTE ring indices the host has not seen yet — the host buffers draws until
 *    the frame's record flush arrives and resolves them at frame end.
 *  - qtmesh_rip_set_interface(NULL) unregisters and MUST be called before the
 *    core is unloaded; the iface ctx must outlive registration.
 */

#ifndef QTMESH_RIP_ABI_H
#define QTMESH_RIP_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QTMESH_RIP_ABI_VERSION 1u

/* Per-vertex shadow delivered with each GP0 draw (filled by the GP0 hook,
 * issue #815). One entry per vertex word of the packet, in packet order. */
typedef struct qtmesh_rip_vertex_shadow {
    float    sx, sy;        /* PGXP precise screen coords (subpixel) */
    float    w;             /* PGXP precise view-space depth (SZ); 0 = unknown */
    uint32_t flags;         /* bit0 = precise xy valid, bit1 = w valid, bit2 = gte_record valid */
    uint32_t gte_record;    /* ring index into the GTE record stream, or UINT32_MAX */
} qtmesh_rip_vertex_shadow;

#define QTMESH_RIP_SHADOW_XY_VALID  (1u << 0)
#define QTMESH_RIP_SHADOW_W_VALID   (1u << 1)
#define QTMESH_RIP_SHADOW_TAG_VALID (1u << 2)

/* Size of the core-side GTE record ring. Part of the ABI: vertex shadows
 * reference ring indices (gte_record ∈ [0, QTMESH_RIP_GTE_RING_ENTRIES)),
 * and hosts derive a record's ring slot as seq % QTMESH_RIP_GTE_RING_ENTRIES.
 * Changing the ring size bumps QTMESH_RIP_ABI_VERSION. */
#define QTMESH_RIP_GTE_RING_ENTRIES (1u << 16)

/* One RTPS/RTPT vertex transform observed inside the GTE (issue #814).
 * Fixed-point conventions match the raw GTE register file: rt entries are
 * s16 4.12 (4096 == 1.0) sign-extended to s32, tr is s32, ofx/ofy are 16.16,
 * h is u16-in-i32. */
typedef struct qtmesh_rip_gte_record {
    int16_t  vx, vy, vz;    /* object-space input vertex (raw GTE V register, s16) */
    int16_t  pad0;
    int32_t  rt[9];         /* rotation matrix R11..R33, 4.12 fixed (GTE cr0-cr4 unpacked) */
    int32_t  tr[3];         /* TRX,TRY,TRZ (cr5-7) */
    int32_t  ofx, ofy;      /* GTE screen offset, 16.16 fixed (cr24, cr25) */
    int32_t  h;             /* projection plane distance (cr26) */
    float    sx, sy, sz;    /* transform outputs: precise screen x/y, view-space z (SZ3) */
    uint32_t frame;         /* core frame counter at transform time */
    uint32_t seq;           /* monotonically increasing record id (ring validity check) */
} qtmesh_rip_gte_record;

/* Host-provided callbacks. All fire synchronously inside retro_run() on the
 * caller's thread. Any pointer argument is only valid for the duration of the
 * call — copy what you need. */
typedef struct qtmesh_rip_host_iface {
    uint32_t abi_version;   /* host fills with QTMESH_RIP_ABI_VERSION */
    void*    ctx;
    /* One executed GP0 command (complete packet: quads are re-assembled from
     * the two-part command-buffer flow before delivery). verts has one entry
     * per vertex word for polygon/sprite packets; 0 for draw-env/line words. */
    void (*on_gp0_draw)(void* ctx, const uint32_t* words, uint32_t word_count,
                        const qtmesh_rip_vertex_shadow* verts, uint32_t vert_count);
    /* Per-frame flush of the GTE record ring (every record captured since the
     * previous flush). Fires before on_frame_end. */
    void (*on_gte_records)(void* ctx, const qtmesh_rip_gte_record* recs, uint32_t count);
    void (*on_frame_end)(void* ctx, uint32_t frame);
} qtmesh_rip_host_iface;

/* Exported by the forked core: */

/* Compile-time ABI version of the loaded core. */
uint32_t qtmesh_rip_abi_version(void);

/* Registers callbacks; iface=NULL unregisters. Returns 0 on success, nonzero
 * on ABI mismatch. The iface struct is copied; ctx must stay valid until
 * unregistration. */
int      qtmesh_rip_set_interface(const qtmesh_rip_host_iface* iface);

/* Arms/disarms capture. Disarmed = hooks compiled in but hot paths take the
 * zero-cost branch and PGXP modes stay user-configured. */
void     qtmesh_rip_set_armed(int armed);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* QTMESH_RIP_ABI_H */
