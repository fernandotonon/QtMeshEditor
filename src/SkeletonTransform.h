/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------------
*/

#ifndef SKELETONTRANSFORM_H
#define SKELETONTRANSFORM_H

#include <Ogre.h>
#include <QString>

class SkeletonTransform
{
public:
    SkeletonTransform();
    static void scaleSkeleton(const Ogre::Entity *_ent, const Ogre::Vector3 &_scale);
    static void translateSkeleton(const Ogre::Entity *_ent, const Ogre::Vector3 &_translate);
    static void rotateSkeleton(const Ogre::Entity *_ent, const Ogre::Vector3 &_rotate);
    static bool renameAnimation(Ogre::Entity *_ent, const QString &_oldName, const QString &_newName);
};

#endif // SKELETONTRANSFORM_H
