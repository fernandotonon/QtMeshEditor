/* Test-only fake libretro core exporting the qtmesh rip ABI (#813/#817).
 *
 * Loaded by LibretroEmuCore via QTMESH_PS1_LIBRETRO_CORE in UnitTests to
 * exercise the full in-core capture chain — handshake, armed mirroring, GTE
 * record delivery, GP0 draw correlation, tiered reconstruction — with zero
 * ROMs/BIOS. Each armed retro_run emits one scripted "scene": a unit cube
 * (8 corners at ±100 model units) drawn as 12 flat triangles whose vertex
 * shadows are tagged with the GTE records that produced them, mimicking the
 * beetle fork's delivery order (draws during the frame, then the record
 * flush, then frame end).
 *
 * Env:
 *   QTMESH_FAKE_RIP_ABI_VERSION  override the reported ABI version
 *                                (handshake-mismatch test)
 */

#include "libretro/libretro_api.h"
#include "libretro/qtmesh_rip_abi.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Libretro plumbing                                                   */
/* ------------------------------------------------------------------ */

static retro_video_refresh_t s_video_cb;
static retro_environment_t s_env_cb;
static uint16_t s_framebuffer[320 * 240];
static uint8_t s_system_ram[64 * 1024];

static qtmesh_rip_host_iface s_iface;
static int s_iface_registered = 0;
static int s_armed = 0;
static uint32_t s_frame = 0;
static uint32_t s_seq = 0;

void retro_init(void) {}
void retro_deinit(void) {}

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name = "QtMesh Fake Rip Core";
    info->library_version = "1";
    info->valid_extensions = "exe|cue|bin|iso";
    info->need_fullpath = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    info->geometry.base_width = 320;
    info->geometry.base_height = 240;
    info->geometry.max_width = 320;
    info->geometry.max_height = 240;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps = 60.0;
    info->timing.sample_rate = 44100.0;
}

void retro_set_environment(retro_environment_t cb) { s_env_cb = cb; (void)s_env_cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { s_video_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { (void)cb; }
void retro_set_input_poll(retro_input_poll_t cb) { (void)cb; }
void retro_set_input_state(retro_input_state_t cb) { (void)cb; }

bool retro_load_game(const struct retro_game_info *game)
{
    (void)game;
    s_frame = 0;
    return true;
}

void retro_unload_game(void) {}

void retro_reset(void)
{
    s_frame = 0;
}

void *retro_get_memory_data(unsigned id)
{
    if (id == RETRO_MEMORY_SYSTEM_RAM)
        return s_system_ram;
    return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
    if (id == RETRO_MEMORY_SYSTEM_RAM)
        return sizeof(s_system_ram);
    return 0;
}

/* ------------------------------------------------------------------ */
/* qtmesh rip ABI                                                      */
/* ------------------------------------------------------------------ */

uint32_t qtmesh_rip_abi_version(void)
{
    const char *override = getenv("QTMESH_FAKE_RIP_ABI_VERSION");
    if (override && override[0])
        return (uint32_t)strtoul(override, NULL, 10);
    return QTMESH_RIP_ABI_VERSION;
}

int qtmesh_rip_set_interface(const qtmesh_rip_host_iface *iface)
{
    if (!iface) {
        memset(&s_iface, 0, sizeof(s_iface));
        s_iface_registered = 0;
        return 0;
    }
    if (iface->abi_version != QTMESH_RIP_ABI_VERSION)
        return 1;
    s_iface = *iface;
    s_iface_registered = 1;
    return 0;
}

void qtmesh_rip_set_armed(int armed)
{
    s_armed = armed ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Scripted scene: a cube at model-space ±100, camera at tr=(0,0,2000) */
/* ------------------------------------------------------------------ */

#define CUBE_H 100
#define CUBE_TRZ 2000
#define CUBE_PROJ_H 256

static const int16_t k_cube_verts[8][3] = {
    {-CUBE_H, -CUBE_H, -CUBE_H}, {CUBE_H, -CUBE_H, -CUBE_H},
    {CUBE_H, CUBE_H, -CUBE_H},   {-CUBE_H, CUBE_H, -CUBE_H},
    {-CUBE_H, -CUBE_H, CUBE_H},  {CUBE_H, -CUBE_H, CUBE_H},
    {CUBE_H, CUBE_H, CUBE_H},    {-CUBE_H, CUBE_H, CUBE_H},
};

static const int k_cube_tris[12][3] = {
    {0, 1, 2}, {0, 2, 3}, /* front  (-z) */
    {5, 4, 7}, {5, 7, 6}, /* back   (+z) */
    {4, 0, 3}, {4, 3, 7}, /* left   (-x) */
    {1, 5, 6}, {1, 6, 2}, /* right  (+x) */
    {4, 5, 1}, {4, 1, 0}, /* top    (-y) */
    {3, 2, 6}, {3, 6, 7}, /* bottom (+y) */
};

static void emit_scripted_scene(void)
{
    /* 36 GTE records (one per triangle vertex, RTPT-style order) followed by
     * 12 tagged flat-triangle GP0 draws, one 0xE1 draw-mode word in-stream,
     * then the frame's record flush + frame end — the fork's delivery order,
     * except draws reference records flushed at frame end (which is exactly
     * what the host must tolerate by buffering draws). */
    qtmesh_rip_gte_record recs[36];
    uint32_t ring_base = s_seq;
    int t, v;

    for (t = 0; t < 12; ++t) {
        for (v = 0; v < 3; ++v) {
            qtmesh_rip_gte_record *rec = &recs[t * 3 + v];
            const int16_t *mv = k_cube_verts[k_cube_tris[t][v]];
            float sz = (float)(mv[2] + CUBE_TRZ);
            memset(rec, 0, sizeof(*rec));
            rec->vx = mv[0];
            rec->vy = mv[1];
            rec->vz = mv[2];
            rec->rt[0] = 4096; /* identity rotation, 4.12 fixed */
            rec->rt[4] = 4096;
            rec->rt[8] = 4096;
            rec->tr[0] = 0;
            rec->tr[1] = 0;
            rec->tr[2] = CUBE_TRZ;
            rec->ofx = 0;
            rec->ofy = 0;
            rec->h = CUBE_PROJ_H;
            rec->sx = (float)mv[0] * CUBE_PROJ_H / sz;
            rec->sy = (float)mv[1] * CUBE_PROJ_H / sz;
            rec->sz = sz;
            rec->frame = s_frame;
            rec->seq = s_seq++;
        }
    }

    /* One draw-mode word before the polygons so per-draw state association is
     * exercised in submission order (tpage 0x1234 & 0x1FF bits land in E1). */
    {
        uint32_t e1 = 0xE1000000u | 0x000000AAu;
        s_iface.on_gp0_draw(s_iface.ctx, &e1, 1, NULL, 0);
    }

    for (t = 0; t < 12; ++t) {
        uint32_t words[4];
        qtmesh_rip_vertex_shadow shadows[3];
        words[0] = 0x20000000u | 0x00808080u; /* flat opaque tri, grey */
        for (v = 0; v < 3; ++v) {
            const qtmesh_rip_gte_record *rec = &recs[t * 3 + v];
            int ix = 160 + (int)rec->sx;
            int iy = 120 + (int)rec->sy;
            words[1 + v] = ((uint32_t)(iy & 0xFFFF) << 16) | (uint32_t)(ix & 0xFFFF);
            shadows[v].sx = rec->sx;
            shadows[v].sy = rec->sy;
            shadows[v].w = rec->sz;
            shadows[v].flags = QTMESH_RIP_SHADOW_XY_VALID | QTMESH_RIP_SHADOW_W_VALID
                               | QTMESH_RIP_SHADOW_TAG_VALID;
            shadows[v].gte_record = (ring_base + (uint32_t)(t * 3 + v))
                                    % QTMESH_RIP_GTE_RING_ENTRIES;
        }
        s_iface.on_gp0_draw(s_iface.ctx, words, 4, shadows, 3);
    }

    if (s_iface.on_gte_records)
        s_iface.on_gte_records(s_iface.ctx, recs, 36);
}

void retro_run(void)
{
    unsigned i;
    for (i = 0; i < 320u * 240u; ++i)
        s_framebuffer[i] = (uint16_t)(0x1F + (s_frame & 0x1F));
    if (s_video_cb)
        s_video_cb(s_framebuffer, 320, 240, 320 * sizeof(uint16_t));

    if (s_iface_registered && s_armed) {
        emit_scripted_scene();
        if (s_iface.on_frame_end)
            s_iface.on_frame_end(s_iface.ctx, s_frame);
    }

    s_frame++;
}
