#pragma once

#include <QString>
#include <QStringList>

// Embeds externally referenced images into a written .glb.
//
// Assimp's glTF2 exporter references every texture by file name (`uri`),
// even in a binary .glb, so the file is not self-contained: moved or shared
// on its own it imports untextured, and viewers that refuse external
// references (web viewers, some engines) show no textures at all. The export
// writes those image files next to the .glb first (exportTextures), so this
// post-process reads each one, appends it to the BIN chunk as a bufferView
// and points the image at it (with its mimeType), then rewrites the file
// atomically.
//
// Qt-only and Ogre-free; unit-tested in GlbImageEmbed_test.cpp.
namespace GlbImageEmbed {

struct Result {
    bool ok = false;          // file parsed and (if anything changed) rewritten
    int embedded = 0;         // images moved into the BIN chunk
    QStringList missing;      // uris whose file could not be read (kept as uri)
    QStringList unsupported;  // not PNG/JPEG and not readable to re-encode
                              // as PNG (e.g. HDR): kept as uri
    QString error;            // set when ok == false
};

// `glbPath` must be a binary glTF. Image uris are resolved relative to the
// .glb's directory (the literal name first, then percent-decoded); `data:`
// uris and images that already use a bufferView are left alone. PNG/JPEG are
// embedded as-is, other readable formats (WebP, BMP, TGA, ...) are re-encoded
// as PNG. A file that needs no
// change is not rewritten.
Result embedExternalImages(const QString& glbPath);

} // namespace GlbImageEmbed
