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

#ifndef MESHDRACOENCODER_H
#define MESHDRACOENCODER_H

#include <QString>

// MeshDracoEncoder — standalone post-processor that adds Draco mesh
// compression (glTF KHR_draco_mesh_compression extension) to a glTF/glb file
// that Assimp has already written.
//
// WHY a standalone module (issue #506, "Path B"): Assimp's glTF2 *exporter*
// has zero Draco support — ASSIMP_BUILD_DRACO=ON only wires the Draco
// *decoder* into the glTF2 importer. So there is no runtime property or export
// flag that makes Assimp write compressed output; the compression has to be
// applied to the finished file. This module reads the glTF JSON + binary
// buffer, replaces each mesh primitive's geometry (indices + POSITION /
// NORMAL / TEXCOORD_n / COLOR_0) with a single Draco-compressed bufferView,
// and rewrites the container. Skins, morph targets, animations, IBM matrices
// and any other accessor data are left byte-for-byte untouched.
//
// The module is Ogre-free and operates purely on the file bytes, so it is
// unit-testable without a GL context. All Draco calls are guarded by
// ENABLE_DRACO; a build without Draco returns a clear error.
class MeshDracoEncoder
{
public:
    // Quantization bits per attribute (Draco defaults, matching gltfpack /
    // gltf-transform conventions). 0 disables quantization for that attribute.
    struct Options {
        int positionBits = 14;
        int normalBits   = 10;
        int texCoordBits = 12;
        int colorBits    = 8;
        int genericBits  = 12;
        // Draco encoding speed 0..10 (0 = best compression / slowest).
        int encodeSpeed = 0;
        int decodeSpeed = 0;
    };

    struct Result {
        bool ok = false;
        QString error;              // human-readable failure reason when !ok
        // True when the file was valid and readable but nothing was eligible for
        // Draco (every primitive was skinned / morph-target / non-float / not an
        // indexed triangle mesh). NOT a hard error — the caller should keep the
        // already-written uncompressed file and warn rather than fail. When set,
        // `ok` is false and `error` explains why nothing was compressed.
        bool nothingEligible = false;
        int primitivesCompressed = 0;
        int primitivesTotal = 0;
        qint64 originalBinBytes = 0;   // size of geometry bytes before compression
        qint64 compressedBinBytes = 0; // size of the Draco bufferViews written
        qint64 originalFileBytes = 0;
        qint64 outputFileBytes = 0;
    };

    // True when the binary was built with Draco encoding support.
    static bool isSupported();

    // Compress in place: reads `path` (.glb or .gltf), applies Draco to every
    // eligible primitive, and writes the result back to `path`. For .gltf the
    // external .bin (if any) is folded in and the output is emitted as a
    // self-contained .glb-style file only when `path` ends in .glb; a .gltf
    // input is rewritten as .gltf with an embedded base64 buffer so the result
    // stays a single file. Returns a Result describing what happened.
    // (Two overloads rather than a `= Options()` default argument: an in-class
    // default argument that value-constructs Options would force the compiler
    // to evaluate Options' implicit constructor — and thus its NSDMIs — before
    // the enclosing class is complete, which is ill-formed.)
    static Result compressFile(const QString& path, const Options& opts);
    static Result compressFile(const QString& path);
};

#endif // MESHDRACOENCODER_H
