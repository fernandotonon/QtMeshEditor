/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/
#ifndef PS1TIM_H
#define PS1TIM_H

#include <OgreImage.h>
#include <QString>

namespace PS1TIM {

/**
 * Decode a PlayStation TIM file to an Ogre::Image (PF_BYTE_RGBA).
 *
 * Supports:
 * - 4bpp indexed (with CLUT)
 * - 8bpp indexed (with CLUT)
 * - 16bpp direct color
 *
 * Notes:
 * - TIM image header width is stored in 16-bit words; pixel width depends on bpp.
 * - For indexed modes, the first CLUT row is used.
 */
bool loadTimToOgreImage(const QString& timPath, Ogre::Image& outImage, QString* outError = nullptr);

} // namespace PS1TIM

#endif

