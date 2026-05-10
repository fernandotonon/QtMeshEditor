/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef PS1RSD_H
#define PS1RSD_H

#include <QString>
#include <QStringList>

/**
 * Sony PlayStation RSD (Resource/Model descriptor) support.
 *
 * This implementation targets the common ASCII variant produced by Psy-Q/SGI tools:
 *
 *   @RSD940102
 *   PLY=MODEL.PLY
 *   MAT=MODEL.MAT
 *   GRP=MODEL.GRP
 *   NTEX=3
 *   TEX[0]=T0.TIM
 *   TEX[1]=T1.TIM
 *   TEX[2]=T2.TIM
 *
 * Notes:
 * - Many engines use RSD as a descriptor that points at companion files (PLY/MAT/GRP and textures).
 * - In QtMeshEditor we treat it as a container that primarily resolves the referenced geometry file and
 *   preloads the referenced TIM textures for best-effort material binding.
 */
namespace PS1RSD {

struct RsdDescriptor {
    QString headerId;      // e.g. "@RSD940102" (optional but expected)
    QString plyPath;       // may point to .ply or other geometry (some pipelines use .tmd here)
    QString matPath;       // optional
    QString grpPath;       // optional
    int ntex{-1};          // optional; -1 = unknown/not present
    QStringList textures;  // TEX[i] entries (usually .tim)

    QStringList geometryCandidates() const {
        QStringList out;
        if (!plyPath.isEmpty())
            out << plyPath;
        return out;
    }
};

/**
 * Parse an ASCII RSD file from disk.
 *
 * @param rsdPath Path to .rsd on disk.
 * @param out Parsed descriptor on success.
 * @param outError Optional error message.
 * @return true on success.
 */
bool parseRsdFile(const QString& rsdPath, RsdDescriptor& out, QString* outError = nullptr);

/**
 * Write an ASCII RSD file to disk.
 *
 * @param rsdPath Path to write.
 * @param desc Descriptor to serialize.
 * @param outError Optional error message.
 * @return true on success.
 */
bool writeRsdFile(const QString& rsdPath, const RsdDescriptor& desc, QString* outError = nullptr);

} // namespace PS1RSD

#endif

