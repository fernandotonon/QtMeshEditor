#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>
#include <vector>

/// #1100: texture the MHR body from a TEXTURED reconstruction of the same person (TRELLIS.2).
///
/// SAM 3D Body gives the geometry and proportions but no appearance. TRELLIS.2 gives an appearance (including the
/// back, which the photo never shows) on its own, differently-proportioned surface. So the MHR body is wrapped onto
/// the TRELLIS surface (similarity + coarse-to-fine RBF warp, normal-gated so a limb cannot grab the other side of the
/// body), and every texel of the MHR UV atlas is filled by looking up the TRELLIS surface at the matching 3D point and
/// sampling its texture there. Pure data: Qt only, no Ogre, no inference.
namespace SamBody {

/// MHR's UV layout: one UV index triple per mesh face (seams are UV-split vertices, not extra positions).
struct UvLayout {
    std::vector<float> texcoords;     ///< nt * 2
    std::vector<int> faceUv;          ///< nf * 3 indices into texcoords
    bool valid() const { return !texcoords.empty() && !faceUv.empty() && faceUv.size() % 3 == 0; }
};

/// File written by scripts/export-sam3dbody-onnx.py: int32 nt, int32 nf, float32[nt*2], int32[nf*3] (little endian).
UvLayout parseUvLayout(const QByteArray& bytes);
UvLayout loadUvLayout(const QString& path);

/// A textured triangle mesh. Positions in any consistent frame; UVs follow the image's own row order (v = row / height),
/// which is what MeshGenPredictor::Result carries.
struct TexturedSurface {
    std::vector<float> positions;     ///< n * 3
    std::vector<uint32_t> indices;    ///< m * 3
    std::vector<float> uvs;           ///< n * 2
    QImage albedo;
    QImage roughness;                 ///< optional, grayscale
    QImage metallic;                  ///< optional, grayscale
};

struct TransferOptions {
    int textureSize = 2048;
    int dilatePx = 24;                ///< fill texels no surface covered from their neighbours (seam gutters, small gaps)
    bool nonRigid = true;             ///< wrap the body onto the surface before sampling (false = similarity only)
    /// Reject a match whose surface normal points away from the body's (the other side of a thin part).
    float minNormalDot = 0.1f;
    /// Reject a match farther than this fraction of the body height (the wrap did not reach it).
    float maxDistanceFraction = 0.06f;
    /// Try facing the source both ways and keep the closer fit (TRELLIS output orientation varies by runtime).
    bool tryBothFacings = true;
};

struct TransferResult {
    bool ok = false;
    QString error;
    QStringList notes;
    QImage albedo;                    ///< RGBA8888, alpha 255 where covered/filled
    QImage roughness, metallic;       ///< only when the source carried them
    float coverage = 0.0f;            ///< fraction of UV-covered texels that matched the surface directly
    float meanDistanceFraction = 0.0f; ///< mean body-to-surface distance after the wrap, / body height
    float yawDegrees = 0.0f;          ///< 0 or 180: how the source was turned to face the body
};

/// `bodyPositions`: nv * 3 (metres, +Y up). `bodyTriangles`: nf * 3 indices into it. `uv`: the layout for those faces.
TransferResult transferTexture(const TexturedSurface& source, const std::vector<float>& bodyPositions,
                               const std::vector<int>& bodyTriangles, const UvLayout& uv,
                               const TransferOptions& options = {});

/// OBJ with the UV layout (v written as 1 - v: OBJ is v-up, the atlas rows are v-down) + a minimal MTL naming `textureFile`.
/// Vertices are UV-split (one per unique position+uv pair), so seams are correct.
QByteArray toTexturedObj(const std::vector<float>& bodyPositions, const std::vector<int>& bodyTriangles, const UvLayout& uv,
                         const QString& mtlFileName, bool facingMinusZ);
/// Self-contained binary glTF: UV-split positions + smooth normals + UVs, the albedo as an embedded PNG base-colour texture and,
/// when roughness and/or metallic maps exist, a packed metallic-roughness texture (G = roughness, B = metallic). The editor reads
/// the texture binding from a glb (it does not from an OBJ's MTL), so this is the format to open in QtMeshEditor.
QByteArray toTexturedGlb(const std::vector<float>& bodyPositions, const std::vector<int>& bodyTriangles, const UvLayout& uv,
                         const QImage& albedo, const QImage& roughness, const QImage& metallic, bool facingMinusZ);
QByteArray toMtl(const QString& materialName, const QString& diffuseFile, const QString& roughnessFile = {},
                 const QString& metallicFile = {});

} // namespace SamBody
