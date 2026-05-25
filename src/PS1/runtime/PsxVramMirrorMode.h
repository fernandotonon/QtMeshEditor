#ifndef PSXVRAMMIRRORMODE_H
#define PSXVRAMMIRRORMODE_H

#include <QString>

/** How the live VRAM snapshot was populated (#660). */
enum class PsxVramMirrorMode {
    Unknown = 0,
    /** Full 1024×512 from RETRO_MEMORY_VIDEO_RAM or core memory map. */
    FullVram,
    /** Visible framebuffer copied to VRAM (0,0); texture pages usually missing. */
    FramebufferFallback,
    /** Framebuffer mirror plus GP0-derived uploads outside the FB rect. */
    Gp0Hybrid,
};

inline QString psxVramMirrorModeLabel(PsxVramMirrorMode mode)
{
    switch (mode) {
    case PsxVramMirrorMode::FullVram:
        return QStringLiteral("full VRAM");
    case PsxVramMirrorMode::FramebufferFallback:
        return QStringLiteral("framebuffer mirror only");
    case PsxVramMirrorMode::Gp0Hybrid:
        return QStringLiteral("framebuffer + GP0 texture patches");
    case PsxVramMirrorMode::Unknown:
    default:
        return QStringLiteral("unknown");
    }
}

#endif // PSXVRAMMIRRORMODE_H
