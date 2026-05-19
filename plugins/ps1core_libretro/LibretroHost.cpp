#include "LibretroHost.h"

#include <QObject>

template<typename T>
bool LibretroHost::resolve(QLibrary &lib, const char *name, T &out, QString *errorOut)
{
    out = reinterpret_cast<T>(lib.resolve(name));
    if (!out) {
        if (errorOut)
            *errorOut = QObject::tr("Libretro core missing symbol: %1").arg(QString::fromLatin1(name));
        return false;
    }
    return true;
}

bool LibretroHost::load(const QString &corePath, QString *errorOut)
{
    unload();

    m_library.setFileName(corePath);
    if (!m_library.load()) {
        if (errorOut)
            *errorOut = m_library.errorString();
        return false;
    }

    const char *symbols[] = {"retro_init",
                             "retro_deinit",
                             "retro_api_version",
                             "retro_get_system_info",
                             "retro_get_system_av_info",
                             "retro_set_environment",
                             "retro_set_video_refresh",
                             "retro_set_audio_sample_batch",
                             "retro_set_input_poll",
                             "retro_set_input_state",
                             "retro_load_game",
                             "retro_unload_game",
                             "retro_run",
                             "retro_reset",
                             "retro_get_memory_data",
                             "retro_get_memory_size"};

    if (!resolve(m_library, symbols[0], retro_init, errorOut) ||
        !resolve(m_library, symbols[1], retro_deinit, errorOut) ||
        !resolve(m_library, symbols[2], retro_api_version, errorOut) ||
        !resolve(m_library, symbols[3], retro_get_system_info, errorOut) ||
        !resolve(m_library, symbols[4], retro_get_system_av_info, errorOut) ||
        !resolve(m_library, symbols[5], retro_set_environment, errorOut) ||
        !resolve(m_library, symbols[6], retro_set_video_refresh, errorOut) ||
        !resolve(m_library, symbols[7], retro_set_audio_sample_batch, errorOut) ||
        !resolve(m_library, symbols[8], retro_set_input_poll, errorOut) ||
        !resolve(m_library, symbols[9], retro_set_input_state, errorOut) ||
        !resolve(m_library, symbols[10], retro_load_game, errorOut) ||
        !resolve(m_library, symbols[11], retro_unload_game, errorOut) ||
        !resolve(m_library, symbols[12], retro_run, errorOut) ||
        !resolve(m_library, symbols[13], retro_reset, errorOut) ||
        !resolve(m_library, symbols[14], retro_get_memory_data, errorOut) ||
        !resolve(m_library, symbols[15], retro_get_memory_size, errorOut)) {
        unload();
        return false;
    }

    return true;
}

void LibretroHost::unload()
{
    retro_init = nullptr;
    retro_deinit = nullptr;
    retro_api_version = nullptr;
    retro_get_system_info = nullptr;
    retro_get_system_av_info = nullptr;
    retro_set_environment = nullptr;
    retro_set_video_refresh = nullptr;
    retro_set_audio_sample_batch = nullptr;
    retro_set_input_poll = nullptr;
    retro_set_input_state = nullptr;
    retro_load_game = nullptr;
    retro_unload_game = nullptr;
    retro_run = nullptr;
    retro_reset = nullptr;
    retro_get_memory_data = nullptr;
    retro_get_memory_size = nullptr;

    if (m_library.isLoaded())
        m_library.unload();
}
