#ifndef CHARACTERGEN_CHARACTERMODEL_H
#define CHARACTERGEN_CHARACTERMODEL_H

// Character generator (pure data, no Ogre): an MHR body (Apache-2.0) with an ICT-FaceKit head (MIT) attached as separate
// parts, built as a `Dna::Rig` so the existing DNA scene importer turns it into a skinned entity with morph targets.
//
//   * body  : mean shape + 45 identity shape vectors; MHR's own skeleton, UVs and skin weights; the MHR head is removed at
//             the neck.
//   * head  : ICT neutral + 100 PCA identity modes (face variety); the 51 ARKit expression shapes become morph targets.
//             Split into material groups (skin / mouth / teeth / sclera / iris / lashes). Rigidly skinned to the head joint,
//             with the neck rows taking the nearest body vertex's weights so the seam deforms with the body.
//
// The two are separate meshes on purpose (a swappable head; no neck weld). Everything here is deterministic and unit-testable
// against a synthetic asset set.

#include "../DNA/DnaReader.h"
#include "QmcBundle.h"

#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>
#include <vector>

namespace CharacterGen {

struct IctPart {
    QString name;
    int v0 = 0, v1 = -1;   ///< vertex index range (inclusive)
    int p0 = 0, p1 = -1;   ///< polygon index range (inclusive)
};

/// Everything read from the two bundles.
struct Assets {
    // MHR body
    int mhrVertices = 0;
    int mhrShapes = 0;
    std::vector<float> mhrBase;              ///< V*3, cm
    std::vector<float> mhrShapeVectors;      ///< S*V*3, cm
    std::vector<int32_t> mhrFaces;           ///< F*3 position indices
    std::vector<int32_t> mhrTexFaces;        ///< F*3 texcoord indices
    std::vector<float> mhrTexcoords;         ///< T*2
    std::vector<int32_t> jointParents;
    std::vector<float> jointOffsets;         ///< J*3, cm, parent-relative at the zero pose
    std::vector<float> jointPrerot;          ///< J*4 (x,y,z,w), parent-relative at the zero pose
    QStringList jointNames;
    std::vector<int32_t> skinCount;          ///< per vertex
    std::vector<int32_t> skinJoint;          ///< concatenated, vertex order
    std::vector<float> skinWeight;
    std::vector<uint32_t> skinStart;         ///< V+1 prefix sums over skinCount (filled by loadAssets)
    std::vector<int32_t> landmarkEyeL, landmarkEyeR, landmarkLip;   ///< optional MHR vertices on the eyelid layer / lip sheet (the eyeballs and mouth follow them)
    std::vector<uint8_t> mhrEyeDrop;         ///< optional: one flag per MHR triangle, the lid skin cut away when the eyes are opened
    std::vector<float> mhrEyeOpen;           ///< optional: V*3 cm, opens the lids of the (shut) MHR neutral so the eyeballs show
    std::vector<float> mhrArkit;             ///< optional: E*V*3 cm, the 51 ARKit shapes expressed on the MHR head (empty in old bundles)
    // ICT head
    int ictVertices = 0;
    int ictIdentities = 0;
    int ictExpressions = 0;
    std::vector<float> ictNeutral;           ///< V*3, cm
    std::vector<float> ictTexcoords;         ///< T*2
    std::vector<int32_t> polySize;
    std::vector<int32_t> polyVertex;         ///< concatenated corner position indices
    std::vector<int32_t> polyTexcoord;       ///< concatenated corner texcoord indices
    std::vector<uint32_t> polyStart;         ///< P+1 prefix sums over polySize (filled by loadAssets)
    std::vector<float> ictIdentity;          ///< I*V*3, cm
    std::vector<float> ictExpression;        ///< E*V*3, cm
    QStringList expressionNames;
    std::vector<IctPart> parts;

    int joint(const QString& name) const { return jointNames.indexOf(name); }
    const IctPart* part(const QString& name) const;
};

/// Parse both bundles (already opened). Validates every size against the others; on any inconsistency returns false with
/// `error` and leaves `out` unspecified.
bool loadAssets(const QmcBundle& mhr, const QmcBundle& ict, Assets& out, QString* error = nullptr);
/// Open `<dir>/mhr_character.qmcb` + `<dir>/ict_character.qmcb`.
bool loadAssetsFromDir(const QString& dir, Assets& out, QString* error = nullptr);

/// Where the optional textured eye lives: `eye.obj` + `iris_<colour>.png` (QTMESH_CHARACTER_EYE_DIR, else
/// `<AppData>/character_eye`). No eye asset ships with the editor; when the folder or the files are missing the ICT eyeballs are used.
QString characterEyeDir();

using Rgb = std::array<float, 3>;   ///< linear-ish 0..1 colour

struct Params {
    std::vector<float> bodyShape;       ///< MHR identity coefficients (45; shorter = rest zero). 0 = the mean body.
    std::vector<float> faceIdentity;    ///< ICT PCA coefficients (100; shorter = rest zero). 0 = the mean face.
    Rgb skin{0.80f, 0.60f, 0.48f};
    Rgb eye{0.22f, 0.36f, 0.55f};
    Rgb lip{0.62f, 0.32f, 0.32f};
    Rgb lash{0.05f, 0.04f, 0.04f};
    /// Bake a procedural skin texture (tone variation, blush, freckles, pores) for the body's own UVs instead of a flat colour.
    /// Only for the native head (the ICT head has a different UV layout).
    bool skinTexture = true;
    int skinTextureSize = 1024;
    /// > 0: keep only the part of the body this many metres below the neck cut and above (head + shoulders; previews/thumbnails).
    float bustDepth = 0.0f;
    /// Keep the MHR body's own head (no neck cut, no ICT head): the expression shapes are MHR's native ones fitted to the ARKit set.
    bool nativeHead = false;
    /// Cut an almond opening through the MHR lids and part them so the eyeballs show. Off: the MHR lids stay as sculpted (eyes shut).
    bool openEyes = false;
    /// Build the skeleton with identity bone orientations (same joint positions): the convention the animation tools assume.
    /// Keep only the standard-named (Mixamo-style) joints: ~52 bones, the skeleton every animation tool expects. Implies identity
    /// bone orientations and standard names. When false the full 127-joint MHR rig is built (with the options below).
    bool standardSkeleton = true;
    bool identityBones = true;
    /// Name the main bones like a Mixamo rig (Hips, Spine, LeftArm, LeftHandIndex1, ...).
    bool standardNames = true;
    /// Fold the procedurally driven helper joints ("_proc" twist joints, "_null") into their real ancestor.
    bool foldHelperJoints = true;
    bool facePlusZ = false;             ///< false = the editor's -Z facing (a turn of 180 degrees about Y)
    /// ICT-space height (cm) where the head's neck is cut. ICT's neck is a short cylinder that flares into shoulders below about
    /// y=-12; cutting above the flare keeps the head's neck a clean ring that can meet the body's own neck.
    float neckRimIct = -11.0f;
    /// How far the head's neck reaches below the cut (m), covering the body's cut ring.
    float neckOverlap = 0.003f;
    /// Body vertices inside the head's neck ring enlarged by this factor (an ellipse around it) count as neck; beyond it they
    /// are shoulders and stay as they are.
    float neckRadiusRatio = 1.0f;
};

/// Deterministic random coefficients: N(0, sigma) per mode (clamped to +-3 sigma), the first `modes` of them only (the leading
/// modes carry the large, readable differences; a long tail of tiny noise only adds weight).
std::vector<float> randomCoefficients(uint32_t seed, int count, float sigma, int modes);

/// "0=1.2,3=-0.5" -> coefficient list (resized to `limit`, untouched entries 0). False + `error` on a malformed or out-of-range item.
bool parseCoefficients(const QString& text, std::vector<float>& out, int limit, QString* error = nullptr);

enum class ColourKind { Skin, Eye };
/// A colour name (skin: pale light tan brown dark; eye: blue green brown hazel grey dark) or any QColor spelling (#rrggbb ...).
bool parseColour(const QString& text, ColourKind kind, Rgb* out);
/// The named colours of a kind, for pickers.
QStringList colourNames(ColourKind kind);

struct BuildReport {
    int bodyVertices = 0, bodyTriangles = 0;
    int headVertices = 0, headTriangles = 0;
    int joints = 0;
    int seamTriangles = 0;           ///< triangles of the strip that stitches the head's neck to the body
    int morphTargets = 0;
    float neckCutY = 0.0f;           ///< world y (m) of the neck cut
    float headScale = 1.0f;
    QStringList meshNames;
    QStringList warnings;
};

/// Build the character. `names` of the produced meshes: "body", "head_skin", "head_mouth", "head_teeth", "head_sclera",
/// "head_iris", "head_lashes". Returns an empty rig (and sets `error`) when the assets are unusable.
Dna::Rig buildRig(const Assets& assets, const Params& params, BuildReport* report = nullptr, QString* error = nullptr);

/// Which material a built mesh takes (by its name) — the scene layer maps this to a tint.
enum class Surface { Skin, Mouth, Teeth, Sclera, Iris, Lashes, Eye };
bool surfaceForMesh(const QString& meshName, Surface* out);

}  // namespace CharacterGen

#endif  // CHARACTERGEN_CHARACTERMODEL_H
