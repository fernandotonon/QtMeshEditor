#pragma once

#include <Ogre.h>
#include <assimp/scene.h>
#include "MaterialProcessor.h"

struct SubMeshData {
    std::vector<Ogre::Vector3> vertices;
    std::vector<Ogre::Vector3> normals;
    std::vector<Ogre::Vector2> texCoords;
    std::vector<Ogre::Vector4> tangents;  // xyz = tangent direction, w = handedness (±1)
    std::vector<Ogre::Vector3> bitangents;
    std::vector<Ogre::ColourValue> colors;
    std::vector<unsigned long> indices;
    std::vector<Ogre::VertexBoneAssignment> boneAssignments;
    unsigned int materialIndex;
};

class MeshProcessor {
public:
    MeshProcessor(Ogre::SkeletonPtr skeleton, bool isZup = false);
    void processNode(aiNode* node, const aiScene* scene);
    Ogre::MeshPtr createMesh(const Ogre::String& name, const Ogre::String& group, MaterialProcessor &materialProcessor);

protected:
    // Protected for testing purposes
    SubMeshData* processMesh(aiMesh* mesh, const aiScene* scene);

private:
    std::vector<SubMeshData*> subMeshesData;
    std::vector<Ogre::VertexBoneAssignment> boneAssignments;
    Ogre::SkeletonPtr skeleton;
    bool m_isZup;
};
