#ifndef LIBRETROEMUCORE_H
#define LIBRETROEMUCORE_H

#include "EmuCore.h"
#include "LibretroHost.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <array>
#include <atomic>

class RipperHooks;

class LibretroEmuCore final : public EmuCore
{
public:
    LibretroEmuCore();
    ~LibretroEmuCore() override;

    QString coreId() const override;
    bool loadBios(const QString &biosPath) override;
    bool loadIso(const QString &isoPath) override;
    bool boot(QString *errorOut) override;
    void runFrame() override;
    void reset() override;
    const EmuFramebuffer &framebuffer() const override;
    void setHooks(EmuHooks *hooks) override;
    void syncCaptureMirrors() override;
    void ingestCaptureFrame() override;
    QString lastError() const override { return m_lastError; }
    void setJoypadButton(unsigned port, unsigned buttonId, bool pressed) override;
    void resetJoypad(unsigned port = 0) override;

    static QString resolveLibretroCorePath(QString *errorOut = nullptr);

private:
    bool ensureInitialized(QString *errorOut);
    bool loadGame(QString *errorOut);
    void unloadGame();
    void applyMemoryMap(const retro_memory_map *map);
    void refreshVramPointer();
    void mirrorFramebufferToVram();
    void syncVramFromCore();
    void captureGpuFromRam();
    void presentVideo(const void *data, unsigned width, unsigned height, size_t pitch);

    static bool environmentCallback(unsigned cmd, void *data);
    static void videoRefreshCallback(const void *data, unsigned width, unsigned height, size_t pitch);
    static void inputPollCallback();
    static int16_t inputStateCallback(unsigned port, unsigned device, unsigned index, unsigned id);

    static LibretroEmuCore *s_active;

    LibretroHost m_host;
    QString m_biosPath;
    QString m_biosLabel;
    QString m_isoPath;
    QString m_loadPath;
    QByteArray m_gamePathUtf8;
    QString m_corePath;
    EmuHooks *m_hooks = nullptr;
    bool m_initialized = false;
    bool m_gameLoaded = false;
    retro_pixel_format m_pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
    bool m_hasVideoFrame = false;
    QString m_lastError;
    QByteArray m_envVarUtf8;
    QByteArray m_envSystemDirUtf8;
    const uint16_t *m_vramPtr = nullptr;
    size_t m_vramBytes = 0;
    bool m_vramUsesFramebufferFallback = false;

    struct MemoryRegion {
        const void *ptr = nullptr;
        size_t len = 0;
        QByteArray addrspaceUtf8;
    };
    QVector<MemoryRegion> m_memoryRegions;

    EmuFramebuffer m_buffers[3];
    int m_writeIndex = 0;
    int m_readIndex = 0;
    quint64 m_frameIndex = 0;

    std::array<std::array<std::atomic<uint16_t>, 16>, 2> m_joypad{};
    int16_t joypadState(unsigned port, unsigned buttonId) const;
};

#endif // LIBRETROEMUCORE_H
