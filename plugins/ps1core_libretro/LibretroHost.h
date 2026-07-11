#ifndef LIBRETROHOST_H
#define LIBRETROHOST_H

#include "libretro/libretro_api.h"
#include "libretro/qtmesh_rip_abi.h"

#include <QLibrary>
#include <QString>

class LibretroHost
{
public:
    bool load(const QString &corePath, QString *errorOut = nullptr);
    void unload();
    bool isLoaded() const { return m_library.isLoaded(); }

    /** True when the loaded core exports the qtmesh rip ABI (#813) — i.e. it
     *  is the rip-instrumented beetle fork. Stock cores return false. */
    bool hasRipInterface() const
    {
        return qtmesh_rip_abi_version && qtmesh_rip_set_interface && qtmesh_rip_set_armed;
    }

    retro_init_t retro_init = nullptr;
    retro_deinit_t retro_deinit = nullptr;
    retro_api_version_t retro_api_version = nullptr;
    retro_get_system_info_t retro_get_system_info = nullptr;
    retro_get_system_av_info_t retro_get_system_av_info = nullptr;
    retro_set_environment_t retro_set_environment = nullptr;
    retro_set_video_refresh_t retro_set_video_refresh = nullptr;
    retro_set_audio_sample_batch_t retro_set_audio_sample_batch = nullptr;
    retro_set_input_poll_t retro_set_input_poll = nullptr;
    retro_set_input_state_t retro_set_input_state = nullptr;
    retro_load_game_t retro_load_game = nullptr;
    retro_unload_game_t retro_unload_game = nullptr;
    retro_run_t retro_run = nullptr;
    retro_reset_t retro_reset = nullptr;
    retro_get_memory_data_t retro_get_memory_data = nullptr;
    retro_get_memory_size_t retro_get_memory_size = nullptr;

    // Optional qtmesh rip ABI (#813). Resolved best-effort after the 16
    // mandatory symbols; nullptr on stock cores (not an error).
    using qtmesh_rip_abi_version_t = uint32_t (*)(void);
    using qtmesh_rip_set_interface_t = int (*)(const qtmesh_rip_host_iface *);
    using qtmesh_rip_set_armed_t = void (*)(int);
    qtmesh_rip_abi_version_t qtmesh_rip_abi_version = nullptr;
    qtmesh_rip_set_interface_t qtmesh_rip_set_interface = nullptr;
    qtmesh_rip_set_armed_t qtmesh_rip_set_armed = nullptr;

private:
    template<typename T>
    bool resolve(QLibrary &lib, const char *name, T &out, QString *errorOut);

    QLibrary m_library;
};

#endif // LIBRETROHOST_H
