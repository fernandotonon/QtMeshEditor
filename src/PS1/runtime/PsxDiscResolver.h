#ifndef PSXDISCRESOLVER_H
#define PSXDISCRESOLVER_H

#include <QString>

struct PsxDiscResolveResult
{
    bool ok = false;
    /** Path passed to libretro retro_load_game (may be a generated .cue). */
    QString loadPath;
    QString errorMessage;
};

/** Maps user-selected disc paths to a libretro-loadable path (Beetle PSX needs .cue for .iso). */
class PsxDiscResolver
{
public:
    static PsxDiscResolveResult resolve(const QString &userPath);

    static bool needsCueWrapper(const QString &path);
};

#endif // PSXDISCRESOLVER_H
