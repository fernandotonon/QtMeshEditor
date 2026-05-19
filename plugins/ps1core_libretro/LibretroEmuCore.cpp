#include "LibretroEmuCore.h"
#include "PsxBiosValidator.h"
#include "PsxDiscResolver.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cstdarg>
#include <cstring>

namespace {

constexpr int kPsxVramWidth = 1024;
constexpr int kPsxVramHeight = 512;
constexpr size_t kPsxVramBytes = static_cast<size_t>(kPsxVramWidth) * kPsxVramHeight * 2;

QStringList libretroCoreCandidates()
{
    QStringList names;
#if defined(Q_OS_WIN)
    names << QStringLiteral("mednafen_psx_libretro.dll")
          << QStringLiteral("beetle_psx_libretro.dll")
          << QStringLiteral("beetle_psx_hw_libretro.dll");
#elif defined(Q_OS_MACOS)
    names << QStringLiteral("mednafen_psx_libretro.dylib")
          << QStringLiteral("beetle_psx_libretro.dylib")
          << QStringLiteral("beetle_psx_hw_libretro.dylib");
#else
    names << QStringLiteral("mednafen_psx_libretro.so")
          << QStringLiteral("beetle_psx_libretro.so")
          << QStringLiteral("beetle_psx_hw_libretro.so");
#endif
    return names;
}

QString envCorePath()
{
    return qEnvironmentVariable("QTMESH_PS1_LIBRETRO_CORE");
}

void prependCoreDirToLibraryPath(const QString &corePath)
{
    const QString coreDir = QFileInfo(corePath).absolutePath();
    const QByteArray existing = qgetenv("LD_LIBRARY_PATH");
    QByteArray merged = QFile::encodeName(coreDir);
    if (!existing.isEmpty()) {
        merged += ':';
        merged += existing;
    }
    qputenv("LD_LIBRARY_PATH", merged);
}

size_t retroAudioSampleBatch(const int16_t *data, size_t frames)
{
    (void)data;
    return frames;
}

void retroLogToQt(enum retro_log_level level, const char *fmt, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    switch (level) {
    case RETRO_LOG_ERROR:
        qWarning().noquote() << "[libretro]" << buffer;
        break;
    case RETRO_LOG_WARN:
        qWarning().noquote() << "[libretro]" << buffer;
        break;
    default:
        qInfo().noquote() << "[libretro]" << buffer;
        break;
    }
}

QString canonicalBiosFileName(const QString &biosLabel)
{
    if (biosLabel.contains(QStringLiteral("5501")))
        return QStringLiteral("scph5501.bin");
    if (biosLabel.contains(QStringLiteral("5502")))
        return QStringLiteral("scph5502.bin");
    if (biosLabel.contains(QStringLiteral("5500")))
        return QStringLiteral("scph5500.bin");
    if (biosLabel.contains(QStringLiteral("1002")))
        return QStringLiteral("scph1002.bin");
    if (biosLabel.contains(QStringLiteral("1000")))
        return QStringLiteral("scph1000.bin");
    return QStringLiteral("scph1001.bin");
}

bool installBiosAliases(const QString &biosPath, const QString &biosLabel)
{
    const QFileInfo info(biosPath);
    const QDir biosDir = info.absoluteDir();
    const QString canonical = canonicalBiosFileName(biosLabel);
    const QString target = biosDir.filePath(canonical);
    if (QFileInfo::exists(target))
        return true;
    if (info.fileName().compare(canonical, Qt::CaseInsensitive) == 0)
        return true;
    if (QFile::link(biosPath, target))
        return true;
    return QFile::copy(biosPath, target);
}

} // namespace

LibretroEmuCore *LibretroEmuCore::s_active = nullptr;

LibretroEmuCore::LibretroEmuCore()
{
    resetJoypad();
    for (EmuFramebuffer &buf : m_buffers) {
        buf.width = 320;
        buf.height = 240;
        buf.rgb24.resize(buf.width * buf.height * 3);
    }
}

LibretroEmuCore::~LibretroEmuCore()
{
    unloadGame();
    if (m_initialized && m_host.retro_deinit) {
        m_host.retro_deinit();
        m_initialized = false;
    }
    m_host.unload();
    if (s_active == this)
        s_active = nullptr;
}

QString LibretroEmuCore::coreId() const
{
    return QStringLiteral("libretro");
}

QString LibretroEmuCore::resolveLibretroCorePath(QString *errorOut)
{
    const QString envPath = envCorePath();
    if (!envPath.isEmpty() && QFileInfo::exists(envPath))
        return QFileInfo(envPath).absoluteFilePath();

    const QString appDir = QCoreApplication::applicationDirPath();
    // Only load cores shipped next to the app (or via env). Distro libretro builds are often
    // too old to load .iso images and can segfault the host process.
    const QStringList searchDirs = {QDir(appDir).filePath(QStringLiteral("PS1Cores"))};

    for (const QString &dirPath : searchDirs) {
        const QDir dir(dirPath);
        if (!dir.exists())
            continue;
        for (const QString &name : libretroCoreCandidates()) {
            const QString path = dir.filePath(name);
            if (QFileInfo::exists(path))
                return QFileInfo(path).absoluteFilePath();
        }
    }

    if (errorOut) {
        *errorOut = QObject::tr(
            "No libretro PSX core found. Install mednafen_psx_libretro (e.g. apt install "
            "libretro-beetle-psx), copy a core .so into PS1Cores/, or set QTMESH_PS1_LIBRETRO_CORE.");
    }
    return {};
}

bool LibretroEmuCore::loadBios(const QString &biosPath)
{
    const QFileInfo info(biosPath);
    if (!info.exists() || !info.isFile())
        return false;

    const PsxBiosValidator::Result check = PsxBiosValidator::validateFile(info.absoluteFilePath());
    if (!check.ok)
        return false;

    m_biosPath = info.absoluteFilePath();
    m_biosLabel = check.label;
    installBiosAliases(m_biosPath, m_biosLabel);
    return true;
}

bool LibretroEmuCore::loadIso(const QString &isoPath)
{
    const QFileInfo info(isoPath);
    if (!info.exists() || !info.isFile())
        return false;

    const QString absolute = info.absoluteFilePath();
    if (m_isoPath == absolute && !m_loadPath.isEmpty())
        return true;

    unloadGame();
    m_isoPath = absolute;

    const PsxDiscResolveResult disc = PsxDiscResolver::resolve(absolute);
    if (!disc.ok) {
        m_loadPath.clear();
        m_lastError = disc.errorMessage;
        return false;
    }

    m_loadPath = disc.loadPath;
    return true;
}

bool LibretroEmuCore::ensureInitialized(QString *errorOut)
{
    if (m_initialized)
        return true;

    m_corePath = resolveLibretroCorePath(errorOut);
    if (m_corePath.isEmpty())
        return false;

    prependCoreDirToLibraryPath(m_corePath);

    if (!m_host.load(m_corePath, errorOut))
        return false;

    s_active = this;
    m_host.retro_set_environment(environmentCallback);
    m_host.retro_set_video_refresh(videoRefreshCallback);
    m_host.retro_set_audio_sample_batch(retroAudioSampleBatch);
    m_host.retro_set_input_poll(inputPollCallback);
    m_host.retro_set_input_state(inputStateCallback);

    m_host.retro_init();
    m_initialized = true;

    if (m_host.retro_get_system_info) {
        retro_system_info si{};
        m_host.retro_get_system_info(&si);
        qInfo().noquote() << "PS1 libretro core:" << (si.library_name ? si.library_name : "?")
                          << (si.library_version ? si.library_version : "")
                          << "path:" << m_corePath;
    }
    return true;
}

bool LibretroEmuCore::loadGame(QString *errorOut)
{
    if (m_gameLoaded)
        return true;

    if (!ensureInitialized(errorOut))
        return false;

    if (m_loadPath.isEmpty()) {
        const PsxDiscResolveResult disc = PsxDiscResolver::resolve(m_isoPath);
        if (!disc.ok) {
            m_lastError = disc.errorMessage;
            if (errorOut)
                *errorOut = m_lastError;
            return false;
        }
        m_loadPath = disc.loadPath;
    }

    m_gamePathUtf8 = m_loadPath.toUtf8();
    retro_game_info game{};
    game.path = m_gamePathUtf8.constData();

    if (!m_host.retro_load_game(&game)) {
        m_lastError = QObject::tr("Libretro core failed to load game: %1").arg(m_loadPath);
        if (errorOut)
            *errorOut = m_lastError;
        return false;
    }

    retro_system_av_info av{};
    m_host.retro_get_system_av_info(&av);
    const unsigned w = av.geometry.base_width ? av.geometry.base_width : 320;
    const unsigned h = av.geometry.base_height ? av.geometry.base_height : 240;

    for (EmuFramebuffer &buf : m_buffers) {
        buf.width = static_cast<int>(w);
        buf.height = static_cast<int>(h);
        buf.rgb24.resize(buf.width * buf.height * 3);
        buf.frameIndex = 0;
    }

    m_gameLoaded = true;
    m_hasVideoFrame = false;
    return true;
}

void LibretroEmuCore::unloadGame()
{
    if (!m_gameLoaded)
        return;
    if (m_host.retro_unload_game)
        m_host.retro_unload_game();
    m_gameLoaded = false;
    m_hasVideoFrame = false;
    m_vramPtr = nullptr;
    m_vramBytes = 0;
}

bool LibretroEmuCore::boot(QString *errorOut)
{
    QString err;
    if (!loadGame(&err)) {
        if (errorOut)
            *errorOut = err;
        return false;
    }

    for (int i = 0; i < 120 && !m_hasVideoFrame; ++i)
        m_host.retro_run();

    if (!m_hasVideoFrame) {
        unloadGame();
        m_lastError = QObject::tr(
            "Libretro core produced no video frames. Check BIOS (SCPH1001.BIN in the BIOS folder), "
            "disc image path, and that mednafen_psx_libretro is installed in PS1Cores/ — run "
            "scripts/install-ps1-libretro-core.sh.");
        if (errorOut)
            *errorOut = m_lastError;
        return false;
    }
    return true;
}

void LibretroEmuCore::runFrame()
{
    QString err;
    if (!loadGame(&err)) {
        if (!err.isEmpty() && m_lastError != err) {
            m_lastError = err;
            qWarning().noquote() << "PS1 libretro:" << err;
        }
        return;
    }

    m_host.retro_run();
    syncVramFromCore();
    captureGpuFromRam();
}

void LibretroEmuCore::reset()
{
    if (m_host.retro_reset && m_gameLoaded)
        m_host.retro_reset();
    m_frameIndex = 0;
    m_hasVideoFrame = false;
}

const EmuFramebuffer &LibretroEmuCore::framebuffer() const
{
    return m_buffers[static_cast<size_t>(m_readIndex)];
}

void LibretroEmuCore::setHooks(EmuHooks *hooks)
{
    m_hooks = hooks;
}

void LibretroEmuCore::syncVramFromCore()
{
    if (!m_hooks)
        return;

    const uint16_t *src = m_vramPtr;
    if (!src || m_vramBytes < kPsxVramBytes)
        return;

    m_hooks->onVramWrite(0, 0, kPsxVramWidth, kPsxVramHeight, src);
}

void LibretroEmuCore::captureGpuFromRam()
{
    if (!m_hooks || !m_hooks->isCaptureEnabled())
        return;

    if (!m_host.retro_get_memory_data || !m_host.retro_get_memory_size)
        return;

    void *ram = m_host.retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    const size_t ramSize = m_host.retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (!ram || ramSize < 4096)
        return;

    m_hooks->ingestSystemRamForGpuCapture(static_cast<const uint8_t *>(ram), ramSize);
}

void LibretroEmuCore::presentVideo(const void *data, unsigned width, unsigned height, size_t pitch)
{
    if (!data || width == 0 || height == 0)
        return;

    EmuFramebuffer &buf = m_buffers[static_cast<size_t>(m_writeIndex)];
    buf.width = static_cast<int>(width);
    buf.height = static_cast<int>(height);
    buf.frameIndex = ++m_frameIndex;
    buf.rgb24.resize(buf.width * buf.height * 3);

    const auto *src = static_cast<const uint8_t *>(data);
    auto *dst = reinterpret_cast<uchar *>(buf.rgb24.data());

    if (m_pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
        for (unsigned y = 0; y < height; ++y) {
            const auto *row = reinterpret_cast<const uint32_t *>(src + y * pitch);
            for (unsigned x = 0; x < width; ++x) {
                const uint32_t px = row[x];
                const int i = static_cast<int>((y * width + x) * 3);
                dst[i + 0] = static_cast<uchar>((px >> 16) & 0xFF);
                dst[i + 1] = static_cast<uchar>((px >> 8) & 0xFF);
                dst[i + 2] = static_cast<uchar>(px & 0xFF);
            }
        }
    } else if (m_pixelFormat == RETRO_PIXEL_FORMAT_RGB565) {
        for (unsigned y = 0; y < height; ++y) {
            const auto *row = reinterpret_cast<const uint16_t *>(src + y * pitch);
            for (unsigned x = 0; x < width; ++x) {
                const uint16_t px = row[x];
                const int i = static_cast<int>((y * width + x) * 3);
                dst[i + 0] = static_cast<uchar>(((px >> 11) & 0x1F) * 255 / 31);
                dst[i + 1] = static_cast<uchar>(((px >> 5) & 0x3F) * 255 / 63);
                dst[i + 2] = static_cast<uchar>(((px >> 0) & 0x1F) * 255 / 31);
            }
        }
    } else {
        for (unsigned y = 0; y < height; ++y) {
            const auto *row = reinterpret_cast<const uint16_t *>(src + y * pitch);
            for (unsigned x = 0; x < width; ++x) {
                const uint16_t px = row[x];
                const int i = static_cast<int>((y * width + x) * 3);
                dst[i + 0] = static_cast<uchar>(((px >> 10) & 0x1F) * 255 / 31);
                dst[i + 1] = static_cast<uchar>(((px >> 5) & 0x1F) * 255 / 31);
                dst[i + 2] = static_cast<uchar>(((px >> 0) & 0x1F) * 255 / 31);
            }
        }
    }

    m_hasVideoFrame = true;
    m_readIndex = m_writeIndex;
    m_writeIndex = (m_writeIndex + 1) % 3;
}

bool LibretroEmuCore::environmentCallback(unsigned cmd, void *data)
{
    LibretroEmuCore *self = s_active;
    if (!self)
        return false;

    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_HW_RENDER:
    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
    case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
        return false;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        if (data)
            *static_cast<bool *>(data) = true;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        if (data) {
            self->m_pixelFormat = *static_cast<retro_pixel_format *>(data);
            return true;
        }
        return false;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        if (!data)
            return false;
        auto *cb = static_cast<retro_log_callback *>(data);
        cb->log = retroLogToQt;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
        if (!data || self->m_biosPath.isEmpty())
            return false;
        self->m_envSystemDirUtf8 = QFileInfo(self->m_biosPath).absolutePath().toUtf8();
        *static_cast<const char **>(data) = self->m_envSystemDirUtf8.constData();
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        return false;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto *var = static_cast<retro_variable *>(data);
        if (!var || !var->key)
            return false;
        if (strcmp(var->key, "beetle_psx_skip_bios") == 0) {
            self->m_envVarUtf8 = "enabled";
            var->value = self->m_envVarUtf8.constData();
            return true;
        }
        if (strcmp(var->key, "beetle_psx_override_bios") == 0) {
            self->m_envVarUtf8 = "disabled";
            var->value = self->m_envVarUtf8.constData();
            return true;
        }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_MEMORY_MAPS: {
        auto *map = static_cast<retro_memory_map *>(data);
        if (!map)
            return false;
        for (unsigned i = 0; i < map->num_descriptors; ++i) {
            const retro_memory_descriptor &d = map->descriptors[i];
            if (d.len >= kPsxVramBytes && d.ptr) {
                self->m_vramPtr = reinterpret_cast<const uint16_t *>(d.ptr);
                self->m_vramBytes = d.len;
                break;
            }
        }
        return true;
    }
    default:
        break;
    }
    return false;
}

void LibretroEmuCore::videoRefreshCallback(const void *data, unsigned width, unsigned height, size_t pitch)
{
    if (s_active)
        s_active->presentVideo(data, width, height, pitch);
}

void LibretroEmuCore::setJoypadButton(unsigned port, unsigned buttonId, bool pressed)
{
    if (port >= m_joypad.size() || buttonId >= m_joypad[port].size())
        return;
    m_joypad[port][buttonId].store(pressed ? 1 : 0, std::memory_order_release);
}

void LibretroEmuCore::resetJoypad(unsigned port)
{
    if (port >= m_joypad.size())
        return;
    for (auto &button : m_joypad[port])
        button.store(0, std::memory_order_release);
}

int16_t LibretroEmuCore::joypadState(unsigned port, unsigned buttonId) const
{
    if (port >= m_joypad.size() || buttonId >= m_joypad[port].size())
        return 0;
    return static_cast<int16_t>(m_joypad[port][buttonId].load(std::memory_order_acquire));
}

void LibretroEmuCore::inputPollCallback() {}

int16_t LibretroEmuCore::inputStateCallback(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (device != RETRO_DEVICE_JOYPAD)
        return 0;
    LibretroEmuCore *self = s_active;
    if (!self)
        return 0;
    return self->joypadState(port, id);
}
