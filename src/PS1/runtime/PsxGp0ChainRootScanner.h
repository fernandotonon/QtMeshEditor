#ifndef PSXGP0CHAINROOTSCANNER_H
#define PSXGP0CHAINROOTSCANNER_H

#include <QSet>
#include <QString>

#include <cstddef>
#include <cstdint>

class EmuHooks;

/**
 * Finds standalone linked GP0 packet chains in main RAM (#657).
 * Complements ordering-table walks when OT heuristics miss retail layouts.
 */
class PsxGp0ChainRootScanner
{
public:
    static int captureFromChainRoots(const uint8_t *ram, size_t byteSize, EmuHooks *hooks,
                                     QSet<QString> *seenPrimKeys, int &primCount);
};

#endif // PSXGP0CHAINROOTSCANNER_H
