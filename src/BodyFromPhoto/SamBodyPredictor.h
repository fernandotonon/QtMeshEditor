#pragma once

#include <QImage>
#include <QRectF>
#include <QString>
#include <QStringList>

#include <array>
#include <memory>
#include <vector>

/// #1100: photo -> human body mesh, via Meta's **SAM 3D Body** (body branch) exported to
/// ONNX by `scripts/export-sam3dbody-onnx.py`, plus the **MHR** (Momentum Human Rig) body model.
///
/// Three graphs (all fp32; parity vs Meta's PyTorch run is ~1e-5 on every output):
///   sam3d_backbone.onnx  image[1,3,512,512] (RGB 0..1)  -> features[1,1280,32,32]
///   sam3d_decoder.onnx   features + ray/cond/affine ... -> shape/scale/pose/camera/model_params
///   mhr_body.onnx        shape[1,45] + model_params[1,204] -> vertices[1,18439,3] (cm) + joints
/// plus `mhr_faces.i32` (36874 triangles, int32 LE). Weights are hosted next to the other models
/// (see THIRD_PARTY_AI_MODELS.md for the licences: the SAM graphs are under the SAM License, the
/// MHR body data is Apache-2.0).
///
/// Ogre-free and Qt-only. The pre-processing (person crop, affine warp, CLIFF condition, camera
/// rays) is pure data and is always compiled and unit-tested; inference is `ENABLE_ONNX` only.
///
/// What is NOT reproduced: MHR pose correctives (a 664 MB network that moves a posed body by a
/// mean 2 mm / max 25 mm) and the face-expression model (the body head zeroes it anyway).
namespace SamBody {

inline constexpr int kCropSize = 512;       ///< backbone input edge
inline constexpr int kFeatureSize = 32;     ///< backbone output edge (stride 16)
inline constexpr int kShapeComps = 45;
inline constexpr int kModelParams = 204;    ///< 136 pose (trans*10, rot, 130 body) + 68 scale
inline constexpr int kPoseParams = 136;
inline constexpr int kVertexCount = 18439;
inline constexpr int kJointCount = 127;     ///< MHR skeleton (see SamBodyMotion.h for the names)

/// Everything the decoder needs to know about where the person is in the photo.
struct CropGeometry {
    bool valid = false;
    std::array<double, 2> center{};       ///< bbox centre in photo px
    double scale = 0.0;                   ///< SQUARE crop edge in photo px (after padding/aspect)
    std::array<float, 6> affine{};        ///< 2x3 photo px -> crop px, row-major (cv2.warpAffine layout)
    float focal = 0.0f;                   ///< assumed focal length in px (sqrt(w^2+h^2) like the reference)
    std::array<float, 2> principal{};     ///< principal point (w/2, h/2)
    std::array<float, 3> cond{};          ///< CLIFF condition (cx-w/2, cy-h/2, b) / focal
    int photoW = 0, photoH = 0;
};

/// bbox = person box in photo px (x1,y1,x2,y2). `padding` is the reference's 1.25. A degenerate
/// box (non-positive size, not finite) returns valid=false.
CropGeometry cropGeometry(int photoW, int photoH, const QRectF& bbox, double padding = 1.25);

/// Warp the photo into the 512x512 crop (bilinear, zero border like cv2) and return CHW floats in
/// 0..1 (RGB). The affine is `geo.affine`.
std::vector<float> warpCrop(const QImage& photo, const CropGeometry& geo);

/// 2 x 512 x 512 ray condition (x then y channel): the camera ray of every crop pixel through the
/// photo's pinhole, exactly the reference's `get_ray_condition`.
std::vector<float> rayCondition(const CropGeometry& geo);

/// Tight box of an alpha matte (alpha >= `threshold`), in photo px; empty QRectF when nothing is
/// foreground. Used to find the person from a background-removed image.
QRectF alphaBounds(const QImage& rgba, int threshold = 128);

struct Body {
    std::vector<float> shape;            ///< 45
    std::vector<float> modelParams;      ///< 204 as predicted (posed)
    std::vector<float> posedVertices;    ///< 18439*3, metres, MHR frame (Y up, +Z front)
    std::vector<float> neutralVertices;  ///< same layout, pose zeroed, shape+scale kept
    std::vector<int> triangles;          ///< 36874*3
    std::vector<float> jointPos;         ///< 127*3, metres, MHR frame (posed)
    std::vector<float> jointQuat;        ///< 127*4 (x,y,z,w) GLOBAL joint rotations, MHR frame (posed)
    std::array<float, 3> camT{};         ///< decoder camera translation (m, camera frame)
    float focalPx = 0.0f;
};

struct Result {
    bool ok = false;
    QString error;
    QString warning;
    Body body;
    CropGeometry geometry;
    QRectF bboxUsed;
    bool usedMatte = false;              ///< the person box came from background removal
    /// false when no box was given and none could be found (the whole photo was used): a motion sequence treats that
    /// frame as a dropout instead of regressing a pose from arbitrary background
    bool personFound = true;
};

struct Options {
    /// Person box in photo px. Empty = find it (background removal) or fall back to the whole photo.
    QRectF bbox;
    bool autoDetect = true;
    /// false = skip the neutral mesh and topology (a motion sequence only needs the joints)
    bool wantMesh = true;
    /// Resolved background-removal model. A caller on a WORKER thread must pass it (resolving may download, which needs the main
    /// thread); empty = resolve here (main-thread callers).
    QString matteModel;
};

QString modelDir();
QString backbonePath();
QString decoderPath();
QString mhrPath();
QString facesPath();
bool modelsPresent();
bool isAvailable();                       ///< built with ENABLE_ONNX
/// Fetch whatever is missing (blocks on the shared downloader). Empty `error` on success.
bool ensureModelsBlocking(QString* error = nullptr);

/// Holds the ONNX sessions between calls: a sequence loads the 3.4 GB backbone once instead of per frame.
class Runner {
public:
    Runner();
    ~Runner();
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;
    Result predict(const QImage& photo, const Options& opts = {});
    /// MHR joint positions (metres) and global rotations (x,y,z,w) at the all-zero pose, the rest pose of the skeleton.
    bool restPose(std::vector<float>& jointPos, std::vector<float>& jointQuat, QString* error = nullptr);
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

/// One-shot pipeline (a temporary Runner). Never returns a partially-valid body.
Result predict(const QImage& photo, const Options& opts = {});

/// Neutral = pose zeroed, shape+scale kept (the body shape of the person).
std::vector<float> neutralParams(const std::vector<float>& modelParams);

/// OBJ text for a body (metres; `facingMinusZ` rotates 180 deg about Y for the editor's -Z-facing convention).
QByteArray toObj(const std::vector<float>& vertices, const std::vector<int>& triangles, bool facingMinusZ);

} // namespace SamBody
