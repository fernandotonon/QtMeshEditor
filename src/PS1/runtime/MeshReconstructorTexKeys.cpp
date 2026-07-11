// Texture group key / material-name helpers, split out of MeshReconstructor.cpp
// so the Ogre-free model-space RAM scanners (PsxTmdRamScanner, #674) can be
// compiled into the libretro plugin without dragging Ogre in.

#include "MeshReconstructor.h"

QString MeshReconstructor::textureMaterialName(uint16_t tpage, uint16_t clut, uint8_t semiTrans,
                                                 uint32_t drawModeBits)
{
    return QStringLiteral("PS1Rip_tpage_%1_clut_%2_st%3_dm%4")
        .arg(tpage, 4, 16, QChar('0'))
        .arg(clut, 4, 16, QChar('0'))
        .arg(semiTrans & 3)
        .arg((drawModeBits >> 11) & 1u);
}

quint64 MeshReconstructor::textureGroupKey(uint16_t tpage, uint16_t clut, uint8_t semiTrans,
                                           uint32_t drawModeBits)
{
    return (static_cast<quint64>(tpage) << 32) | clut
           | (static_cast<quint64>(semiTrans & 3) << 48)
           | (static_cast<quint64>((drawModeBits >> 11) & 1u) << 52);
}
