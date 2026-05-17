#ifndef EMUFRAMEBUFFER_H
#define EMUFRAMEBUFFER_H

#include <QByteArray>
#include <QtGlobal>

/** Immutable RGB24 emulator output snapshot (width * height * 3 bytes). */
struct EmuFramebuffer
{
    int width = 0;
    int height = 0;
    quint64 frameIndex = 0;
    QByteArray rgb24;

    bool isValid() const { return width > 0 && height > 0 && rgb24.size() == width * height * 3; }
};

#endif // EMUFRAMEBUFFER_H
