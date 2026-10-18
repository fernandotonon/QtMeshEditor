#ifndef DNA_DNAREADER_H
#define DNA_DNAREADER_H

// MetaHuman DNA import (epic #1095, Phase 1): a pure-data reader on top of
// Epic's OpenRigLogic DNA library.
//
// Everything here is Ogre-free and Qt-GUI-free so the whole reader is unit-
// testable headless. The Ogre side lives in DnaScene.{h,cpp}. All OpenRigLogic
// symbols stay inside DnaReader.cpp behind `#ifdef ENABLE_OPENRIGLOGIC`; a
// build without the flag compiles, and every entry point fails with a clear
// "rebuild with -DENABLE_OPENRIGLOGIC=ON" message instead of crashing.
//
// Scope note (Phase 1): this reads the DEFINITION + GEOMETRY layers into plain
// arrays and reports which BEHAVIOR layers the file carries (so the Phase 2
// body-corrective work knows what it has to evaluate). It does not evaluate
// the rig — that is RigLogic's job and a separate slice.

#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>
#include <vector>

namespace Dna {

/// True only when built with ENABLE_OPENRIGLOGIC.
bool available();

/// Why a read failed — lets callers (CLI/GUI/MCP) give an ACTIONABLE message
/// instead of the library's terse status text.
enum class ErrorCode {
    None = 0,
    Unavailable,         ///< built without ENABLE_OPENRIGLOGIC
    FileNotFound,        ///< path missing / unreadable
    NotDna,              ///< signature mismatch — not a binary .dna file
    UnsupportedVersion,  ///< a DNA generation this build's library cannot read
    Corrupt,             ///< truncated / inconsistent data
    MissingGeometry,     ///< valid DNA but no geometry layer (a behaviour-only file)
    InvalidLod,          ///< requested LOD is not in the file
    Other
};

enum class Units { Preserve, Meters };

/// Space the DNA's neutral joint transforms are expressed in. MetaHuman DNA
/// stores them PARENT-RELATIVE (verified on Epic's SKM_Body/SKM_Face: a hand is
/// 0.007 from its parent, a thigh 0.096 from the pelvis; accumulating them down
/// the hierarchy reproduces the mesh's pose). The root joint carries a -90deg X
/// orientation that turns the data's Z-up joint axes into Y-up. `World` exists
/// for a producer that writes world transforms.
enum class NeutralSpace { ParentRelative, World };

struct ReadOptions {
    int lod = 0;                         ///< LOD whose meshes/joints the result exposes (0 = highest)
    bool loadGeometry = true;            ///< vertices, faces, skin weights, blend-shape targets
    bool loadBehavior = false;           ///< count rig-logic data (controls, PSD, RBF, …) — info/Phase 2
    Units units = Units::Meters;         ///< cm -> m for the MetaHuman default
    bool convertCoordinateSystem = true; ///< library-side transform to right-handed, Y-up, character facing +Z
    NeutralSpace neutralSpace = NeutralSpace::ParentRelative;
    bool flipV = true;                   ///< DNA UVs are bottom-left origin; Ogre/glTF are top-left
};

struct Descriptor {
    QString name;
    QString archetype;
    QString gender;
    int age = 0;
    QString translationUnit;      ///< as authored ("cm" / "m")
    QString rotationUnit;         ///< as authored ("degrees" / "radians")
    QString coordinateSystem;     ///< as authored, e.g. "x=right y=up z=front"
    QString rotationSequence;     ///< as authored, e.g. "xyz"
    QString faceWindingOrder;     ///< "ccw" / "cw"
    int lodCount = 0;
    QString dbName;
    QString dbComplexity;
    QStringList metadata;         ///< "key=value"
};

/// What rig-logic data the file carries — the Phase 2 inventory.
struct BehaviorSummary {
    bool loaded = false;
    int guiControls = 0;
    int rawControls = 0;
    int psdCount = 0;             ///< pose-space-deformer rows (the corrective drivers)
    int jointGroups = 0;
    int blendShapeChannels = 0;
    int animatedMaps = 0;
    int rbfPoses = 0;             ///< RBF corrective poses
    int neuralNetworks = 0;
    int twists = 0;
    int swings = 0;
};

struct Joint {
    QString name;
    int parent = -1;                       ///< -1 = root
    std::array<double, 3> translation{};   ///< LOCAL (parent-relative), in the requested units
    std::array<double, 4> orientation{1, 0, 0, 0};  ///< LOCAL quaternion (w,x,y,z)
};

struct BlendShapeTarget {
    int channel = -1;                      ///< index into Rig::blendShapeChannels
    std::vector<uint32_t> vertexIndices;   ///< POSITION indices the deltas apply to
    std::vector<float> deltas;             ///< 3 floats per entry, in the requested units
};

struct SkinInfluence {
    uint16_t joint = 0;
    float weight = 0.0f;
};

struct Mesh {
    QString name;
    int dnaIndex = -1;                     ///< index in the file's mesh table
    std::vector<float> positions;          ///< 3 per position
    std::vector<float> normals;            ///< 3 per normal
    std::vector<float> uvs;                ///< 2 per texture coordinate
    /// One entry per distinct (position, uv, normal) corner combination.
    struct Layout { uint32_t position = 0, uv = 0, normal = 0; };
    std::vector<Layout> layouts;
    /// Faces as lists of layout indices (any vertex count >= 3).
    std::vector<std::vector<uint32_t>> faces;
    /// Skin weights per POSITION vertex (empty when the mesh is unskinned).
    std::vector<std::vector<SkinInfluence>> skin;
    std::vector<BlendShapeTarget> blendShapeTargets;
};

struct Rig {
    Descriptor descriptor;
    BehaviorSummary behavior;
    std::vector<Joint> joints;             ///< ALL joints (the skeleton is not LOD-pruned here)
    std::vector<int> lodJointIndices;      ///< joints active at the requested LOD
    std::vector<Mesh> meshes;              ///< meshes at the requested LOD
    /// Joints the rig reads or moves (driver + corrective joints), when known —
    /// filled by the importer from RigLogic so a skeleton that must be reduced
    /// folds everything else first. Empty = no information.
    std::vector<char> protectedJoint;
    QStringList blendShapeChannels;
    QStringList guiControls;
    QStringList rawControls;
    QStringList animatedMaps;
    int lod = 0;
    bool flipV = true;                     ///< expand() should flip V (see ReadOptions::flipV)
    std::array<double, 3> unitScale{1, 1, 1};  ///< applied to translations/positions (informational)
    bool coordinateSystemConverted = false;
    QStringList warnings;                  ///< non-fatal gaps (dropped LOD data, unnamed meshes, …)
};

struct ReadResult {
    bool ok = false;
    ErrorCode code = ErrorCode::None;
    QString error;                         ///< actionable, user-facing
    Rig rig;
};

/// Read a binary `.dna` file. Pure data — no Ogre, no GL.
ReadResult read(const QString& path, const ReadOptions& options = ReadOptions());

/// Read from memory (tests, embedded assets). Same semantics as read().
ReadResult readFromBuffer(const std::vector<uint8_t>& bytes, const ReadOptions& options = ReadOptions());

// ---- pure helpers (always compiled; unit-tested without the library) -------

/// Euler angles -> quaternion (w,x,y,z), composing the three axis rotations in
/// `sequence` order as an intrinsic product (`xyz` => Rx*Ry*Rz) — the same
/// convention OpenRigLogic's own tdm::euler_to_quat uses. `signs` flips the
/// positive rotation direction per axis (+1 / -1).
std::array<double, 4> eulerToQuaternion(const std::array<double, 3>& angles,
                                        const QString& sequence,
                                        bool degrees,
                                        const std::array<int, 3>& signs = {1, 1, 1});

/// Convert WORLD-space joint transforms to parent-relative ones in place.
/// `joints[i].translation/orientation` are world on entry and local on exit.
/// Requires parents to be resolvable; a parent index >= i is processed via a
/// topological order, so any parent ordering is accepted. Returns false on a
/// hierarchy cycle.
bool worldToLocal(std::vector<Joint>& joints);

/// Flat, GPU-ready expansion of a Mesh: one vertex per layout entry.
struct ExpandedMesh {
    std::vector<float> positions;   ///< 3 per vertex
    std::vector<float> normals;     ///< 3 per vertex
    std::vector<float> uvs;         ///< 2 per vertex
    std::vector<uint32_t> indices;  ///< triangle list (faces fan-triangulated)
    std::vector<uint32_t> positionOf;  ///< vertex -> source POSITION index
    /// Skin weights per expanded vertex (copied from its position vertex).
    std::vector<std::vector<SkinInfluence>> skin;
    int skippedFaces = 0;           ///< faces dropped for < 3 corners or out-of-range indices
};

/// Expand a mesh's layout-indexed corners into a flat vertex list. Out-of-range
/// indices are reported via `skippedFaces`, never dereferenced.
ExpandedMesh expand(const Mesh& mesh, bool flipV);

}  // namespace Dna

#endif  // DNA_DNAREADER_H
