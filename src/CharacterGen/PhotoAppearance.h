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
    int hairLength = 0;
    float sideHairFraction = 0.0f;
    bool faceFound = false;
    QRectF faceBox;          ///< photo px
    QString note;
};

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

}  // namespace PhotoAppearance
}  // namespace CharacterGen

#endif
