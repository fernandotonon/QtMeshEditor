#ifndef PHOTOAPPEARANCE_H
#define PHOTOAPPEARANCE_H

// Classical (no training) appearance estimation from a photo of a person: skin tone, hair colour, iris colour.
// Pure-data core (QImage + point lists in, colours out; unit-tested without models) and an orchestrator that finds the
// face (MediaPipe landmarks, FaceRig) and the person matte (BackgroundRemover) in the photo.

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <array>
#include <vector>

namespace CharacterGen {
namespace PhotoAppearance {

using Rgb = std::array<float, 3>;   // sRGB 0..1, like CharacterGen::Params colours

struct ColourEstimate {
    bool ok = false;
    Rgb colour{};
    int samples = 0;       ///< pixels that voted
    float spread = 0.0f;   ///< median Lab distance of the voters to the result (lower = a cleaner estimate)
};

/// sRGB (0..255 per channel) -> CIE Lab (D65).
std::array<float, 3> toLab(float r, float g, float b);

/// Robust colour of the pixels where `mask` (any format, alpha or luminance >= 128) is set: Lab median, then a second median over
/// the pixels within `trimDeltaE` of it. `minSamples` below which ok = false.
ColourEstimate robustColour(const QImage& image, const QImage& mask, float trimDeltaE = 18.0f, int minSamples = 40);

/// Same, over an explicit pixel list (RGB 0..255).
ColourEstimate robustColour(const std::vector<std::array<uint8_t, 3>>& pixels, float trimDeltaE = 18.0f, int minSamples = 40);

/// Filled polygon -> 8-bit mask of size w x h (255 inside).
QImage polygonMask(int w, int h, const std::vector<QPointF>& polygon);

/// Polygon scaled about its centroid.
std::vector<QPointF> scaledPolygon(const std::vector<QPointF>& polygon, double factor);

struct FaceAppearance {
    ColourEstimate skin, hair, iris;
    /// 0 = no hair found (bald / hat), 1 = short, 2 = chin length, 3 = long (hair hanging beside the neck)
    std::vector<std::array<float, 3>> landmarks;   ///< the 478 MediaPipe landmarks in photo px (x, y, relative z), empty when no face
    int hairLength = 0;
    float sideHairFraction = 0.0f;
    bool faceFound = false;
    QRectF faceBox;          ///< photo px
    QString note;
};

struct FaceFit {
    bool ok = false;
    std::vector<float> coefficients;   ///< the 45 MHR shape coefficients after the fit (only the face modes changed)
    float errBefore = 0.0f, errAfter = 0.0f;   ///< mean landmark error / inter-ocular distance in the model's face space
    int modesUsed = 0;
    QString note;
};

/// Refine SAM's 45 coefficients so the MHR head's MediaPipe landmarks match the photo's (3D-similarity aligned): a linear model
/// landmarks(c) = L0 + A c, regressed on 160 rendered identities (resources/face_landmark_fit.bin, detector = face_landmarks.onnx),
/// solved by ridge towards SAM's values, restricted to the `modes` coefficients that move the face most. Measured on 16 portraits
/// (re-rendered and re-detected): landmark error 0.0431 -> 0.0377 of the inter-ocular distance, better on 16/16; the mean face scores 0.0426.
FaceFit fitFaceCoefficients(const std::vector<std::array<float, 3>>& photoLandmarks, const std::vector<float>& sam, int modes = 12, float lambda = 0.05f);

/// Everything from the 478 MediaPipe landmarks of one face (photo px) + a person matte (255 = person; may be empty = unknown).
FaceAppearance fromLandmarks(const QImage& photo, const std::vector<std::array<float, 3>>& landmarks, const QImage& personMatte);

/// Photo-px box of the head from SAM's posed MHR vertices (metres, MHR frame, Y up): the topmost `headHeight` metres are projected through
/// the decoder's pinhole camera (camT, focal in photo px, principal point at the photo centre).
QRectF headBoxFromBody(const std::vector<float>& posedVertices, const std::array<float, 3>& camT, float focalPx, int photoW, int photoH, float headHeight = 0.25f);

struct ClothingEstimate {
    bool ok = false;
    QString top = QStringLiteral("none");      ///< none | tshirt | longsleeve | tank (the ids of Clothing.h)
    QString bottom = QStringLiteral("none");   ///< none | shorts | pants
    ColourEstimate topColour, bottomColour;
    QString note;
};

/// Clothes from SAM's posed body: body regions (torso, upper arm, forearm, thigh, shin) are projected into the photo, intersected with the
/// person matte, and compared with the skin tone — a region whose colour is far from the skin is covered by a garment.
ClothingEstimate estimateClothing(const QImage& photo, const QImage& personMatte, const Rgb& skin, const std::vector<float>& neutralVertices,
                                  const std::vector<float>& posedVertices, const std::vector<int>& triangles, const std::array<float, 3>& camT, float focalPx);

/// Person matte in photo px (255 = person), empty when unavailable.
QImage personMatte(const QImage& photo);

/// Full pipeline: photo + person box (photo px; empty = whole photo) -> face found by landmarks, matte by BackgroundRemover.
FaceAppearance estimate(const QImage& photo, const QRectF& personBox, const QRectF& headBox = QRectF(), QImage* matteOut = nullptr);

/// Where each MHR vertex of the PERSON'S posed mesh lands in the photo, and how well the photo sees it (facing the camera, inside the
/// head box, on the person matte; with the face's landmarks, inside the face oval so hair, ears and the neck keep the procedural skin). The character's skin bake maps these onto its own body by vertex id.
struct FaceProjection {
    bool ok = false;
    std::vector<float> pixel;    ///< 2 per vertex
    std::vector<float> weight;   ///< 1 per vertex, 0..1
    int visible = 0;             ///< vertices with weight > 0.5
};
FaceProjection projectFace(const QImage& photo, const QImage& personMatte, const QRectF& headBox, const std::vector<float>& posedVertices,
                           const std::vector<int>& triangles, const std::array<float, 3>& camT, float focalPx,
                           const std::vector<std::array<float, 3>>& landmarks = {});

}  // namespace PhotoAppearance
}  // namespace CharacterGen

#endif
