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

    /** Decodes the next GP0 packet at @p words[0]; returns @c wordsConsumed (0 if none). */
    struct Gp0Step {
        size_t wordsConsumed = 0;
        bool hasPrim = false;
        PrimRecord prim{};
        bool hasDrawMode = false;
        DrawModeRecord drawMode{};
        bool hasVramWrite = false;
        uint16_t vramX = 0;
        uint16_t vramY = 0;
        uint16_t vramW = 0;
        uint16_t vramH = 0;
        const uint16_t *vramPixels = nullptr;
        bool hasVramRead = false;
        uint16_t vramReadX = 0;
        uint16_t vramReadY = 0;
        uint16_t vramReadW = 0;
        uint16_t vramReadH = 0;
        QString error;
    };
    static Gp0Step stepGp0(const uint32_t *words, size_t wordCount);

    /** Serialize primitives to CSV for regression dumps (tests / debug). */
    static QString primsToCsv(const QVector<PrimRecord> &prims);
};

#endif // GPUCOMMANDPARSER_H
