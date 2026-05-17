#ifndef GPUCOMMANDPARSER_H
#define GPUCOMMANDPARSER_H

#include "CaptureTypes.h"

#include <QVector>
#include <QString>
#include <cstddef>
#include <cstdint>

/**
 * Decodes PS1 GP0 command streams into PrimRecord / DrawModeRecord (#418).
 * Reference: nocash psx-spx — GPU Rendering Polygon Commands.
 */
class GpuCommandParser
{
public:
    struct ParseResult {
        QVector<PrimRecord> prims;
        QVector<DrawModeRecord> drawModes;
        QString error;
    };

    static ParseResult parseGp0(const uint32_t *words, size_t wordCount);

    /** Serialize primitives to CSV for regression dumps (tests / debug). */
    static QString primsToCsv(const QVector<PrimRecord> &prims);
};

#endif // GPUCOMMANDPARSER_H
