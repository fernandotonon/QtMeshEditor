#pragma once

#include "BodyTexture.h"
#include "SamBodyPredictor.h"
#include "../ImageTo3D/MeshGenPredictor.h"

#include <QImage>
#include <QString>

namespace SamBody {

struct TexturedBodyOptions {
    int textureSize = 2048;
    QString trellisPreset = QStringLiteral("balanced");   ///< fast | balanced | high
    bool bakePbr = true;                                  ///< carry TRELLIS's roughness / metallic maps over
    TransferOptions transfer;
    /// Forwarded to TRELLIS.2 (stage, done, total); return false to cancel. Empty = none.
    MeshGenPredictor::ProgressFn progress;
};

struct TexturedBodyResult {
    bool ok = false;
    QString error;
    QStringList notes;
    TransferResult transfer;
};

/// Run TRELLIS.2 on `photo` and bake its appearance onto `body` (its neutral or posed vertices, the same triangles and
/// `uv`). Fails with a clear message when the TRELLIS.2 runtime is not installed or its bake carries no texture.
TexturedBodyResult textureBodyFromTrellis(const QImage& photo, const std::vector<float>& bodyVertices,
                                          const std::vector<int>& bodyTriangles, const UvLayout& uv,
                                          const TexturedBodyOptions& options = {});

/// `.glb` path: one self-contained binary glTF (what the editor binds textures from). Otherwise `<stem>.obj`, `<stem>.mtl` and the texture PNGs next to each other. Returns an error string (empty on success).
QString writeTexturedBody(const QString& objPath, const std::vector<float>& vertices, const std::vector<int>& triangles,
                          const UvLayout& uv, const TransferResult& tex, bool facingMinusZ);

} // namespace SamBody
