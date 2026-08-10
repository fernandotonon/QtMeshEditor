#pragma once

#include <Ogre.h>
#include <assimp/scene.h>
#include "MaterialProcessor.h"

/// One blend-shape / morph target on a SubMeshData. Stored as the
/// absolute per-vertex positions of the deformed shape (matches what
/// Assimp gives us via `aiAnimMesh::mVertices`); MeshProcessor
/// converts these to per-vertex deltas relative to `vertices` when
/// creating the Ogre::Pose entries at mesh-build time.
struct MorphTargetData {
    std::string name;                       ///< From `aiAnimMesh::mName`; empty when the source had no name.
    std::vector<Ogre::Vector3> positions;   ///< Absolute deformed positions, same vertex order as `SubMeshData::vertices`.
};

struct SubMeshData {
    std::vector<Ogre::Vector3> vertices;
    std::vector<Ogre::Vector3> normals;
    std::vector<Ogre::Vector2> texCoords;
    std::vector<Ogre::Vector4> tangents;  // xyz = tangent direction, w = handedness (±1)
    std::vector<Ogre::Vector3> bitangents;
    std::vector<Ogre::ColourValue> colors;
    std::vector<unsigned long> indices;
    std::vector<Ogre::VertexBoneAssignment> boneAssignments;
    std::vector<MorphTargetData> morphTargets;  ///< Empty when source had no blend shapes.
    unsigned int materialIndex;
    std::string name;  ///< From `aiMesh::mName`; drives Mesh::nameSubMesh so
                       ///< named submeshes (e.g. PartOps parts) survive import.
};

class MeshProcessor {
public:
    MeshProcessor(Ogre::SkeletonPtr skeleton, bool isZup = false,
                  const Ogre::Quaternion& bakeRotation = Ogre::Quaternion::IDENTITY);
    void processNode(aiNode* node, const aiScene* scene);
    Ogre::MeshPtr createMesh(const Ogre::String& name, const Ogre::String& group, MaterialProcessor &materialProcessor);

    // Morph-target name hints from a `<file>.arkit.json` sidecar (ordered
    // names). Assimp's glTF2 EXPORTER drops `targetNames`, so a re-imported
    // rigged glb otherwise degrades to generated "Shape_N" names — losing the
    // ARKit vocabulary face capture matches on. Applied only when the aiMesh
    // itself carries no names and the scene has a single morphed mesh (the
    // unambiguous case — e.g. the ARKit reference head).
    void setMorphNameHints(std::vector<std::string> names) { m_nameHints = std::move(names); }

protected:
    // Protected for testing purposes
    SubMeshData* processMesh(aiMesh* mesh, const aiScene* scene,
                             const aiNode* node = nullptr);

private:
    std::vector<SubMeshData*> subMeshesData;
    std::vector<Ogre::VertexBoneAssignment> boneAssignments;
    // #933: bind a RIGID (bone-less) mesh that is node-parented under a bone
    // (Blender bone-parented objects — Quaternius Robot) to that bone with
    // weight 1, baking its node chain relative to the bone into the vertices.
    // Renders assembled AND animates correctly; without it Ogre's software
    // vertex blend asserts (missing blend elements) on any render.
    void bindRigidMeshToParentBone(SubMeshData* data, const aiNode* node);
    // #933: alignment of a skinned mesh into the reference mesh's frame.
    // Returns true when the transform is meaningfully non-identity.
    bool computeFrameAlign(const aiNode* node, Ogre::Matrix4& frameAlign,
                           Ogre::Quaternion& frameAlignRot) const;
    Ogre::SkeletonPtr skeleton;
    bool m_isZup;
    // Rest-pose rotation baked into vertices/normals/tangents (identity when
    // none). Set from Z-up metadata (+90°X) or the skinned mesh node's world
    // orientation for Blender-style rigs (#933).
    Ogre::Quaternion m_bakeRot = Ogre::Quaternion::IDENTITY;
    bool m_hasBake = false;
    // #933: multi-mesh skinned scenes — Assimp bone offsets are relative to
    // EACH mesh's own node, but the Ogre skeleton is built against the FIRST
    // skinned mesh's node frame. Align every other skinned mesh into that
    // reference frame (C = refWorld⁻¹ · thisMeshNodeWorld) so parts assemble.
    Ogre::Matrix4 m_refWorldInv = Ogre::Matrix4::IDENTITY;
    bool m_haveRef = false;
    std::vector<std::string> m_nameHints;
};
