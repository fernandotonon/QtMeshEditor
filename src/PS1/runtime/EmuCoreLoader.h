#ifndef EMUCORELOADER_H
#define EMUCORELOADER_H

#include "EmuCore.h"

#include <QString>
#include <memory>

/**
 * Loads an EmuCore implementation from <app>/PS1Cores/*.so at runtime.
 */
class EmuCoreLoader
{
public:
    static QStringList coreSearchPaths();
    static std::unique_ptr<EmuCore> loadCore(QString *errorOut = nullptr);
};

#endif // EMUCORELOADER_H
