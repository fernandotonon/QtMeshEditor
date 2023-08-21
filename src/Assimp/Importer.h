/*/////////////////////////////////////////////////////////////////////////////////
/// A QtMeshEditor file
///
/// Copyright (c)
///
/// The MIT License
///
/// Permission is hereby granted, free of charge, to any person obtaining a copy
/// of this software and associated documentation files (the "Software"), to deal
/// in the Software without restriction, including without limitation the rights
/// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
/// copies of the Software, and to permit persons to whom the Software is
/// furnished to do so, subject to the following conditions:
///
/// The above copyright notice and this permission notice shall be included in
/// all copies or substantial portions of the Software.
///
/// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
/// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
/// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
/// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
/// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
/// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
/// THE SOFTWARE.
////////////////////////////////////////////////////////////////////////////////*/

#pragma once

#include <Ogre.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "MaterialProcessor.h"

struct SubMeshData {
    std::vector<Ogre::Vector3> vertices;
    std::vector<Ogre::Vector3> normals;
    std::vector<Ogre::Vector2> texCoords;
    std::vector<Ogre::Vector3> tangents;
    std::vector<Ogre::Vector3> bitangents;
    std::vector<Ogre::ColourValue> colors;
    std::vector<unsigned long> indices;
    std::vector<Ogre::Vector4> blendIndices;
    std::vector<Ogre::Vector4> blendWeights;
    std::vector<Ogre::VertexBoneAssignment> boneAssignments;
    unsigned int materialIndex;
    std::string mName;
    std::map<std::string, aiBone*> mapAiBone;
};

struct BoneNode {
    aiBone* bone;
    std::vector<BoneNode*> children;
};

class AssimpToOgreImporter {
public:
    AssimpToOgreImporter() : importer() {}

    Ogre::MeshPtr loadModel(const std::string& path);

private:
    void processNode(aiNode* node, const aiScene* scene);

    // Bones
    void createOgreBones(const aiScene *scene);
    void createBone(const std::string& boneName);
    void processBoneHierarchy(aiBone* bone, const aiScene* scene);
    void processBoneNode(aiBone *bone, SubMeshData& subMeshData);

    // Animations
    void processAnimations(const aiScene* scene);
    void processAnimation(aiAnimation* animation, const aiScene* scene);
    void processAnimationChannel(aiNodeAnim* nodeAnim, Ogre::Animation* animation, const aiScene* scene, unsigned int channelIndex);

    // Mesh
    Ogre::MeshPtr createMesh();
    SubMeshData* processMesh(aiMesh* mesh, const aiScene* scene);

    Assimp::Importer importer;
    std::vector<SubMeshData*> subMeshesData;
    Ogre::SkeletonPtr skeleton;
    std::map<std::string, aiBone*> aiBonesMap;
    std::map<std::string, std::vector<SubMeshData*>> boneNameToSubMeshes;
    std::vector<Ogre::VertexBoneAssignment> boneAssignments;
    std::string modelName;

    MaterialProcessor materialProcessor;
};
