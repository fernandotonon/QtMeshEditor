#pragma once

#include <Ogre.h>
#include <assimp/scene.h>

class BoneProcessor {
    public:
        void processBones(Ogre::SkeletonPtr skeleton, const aiScene* scene, bool isZup = false);

    private:
        void createBone(const std::string& boneName);
        void processBoneHierarchy(aiNode* node);
        void processBoneNode(aiBone *bone);
        void processNonSkinnedBone(aiNode* node);
        void processAnimationOnlyHierarchy(aiNode* node, const std::set<std::string>& animatedNodes);
        Ogre::Matrix4 convertToOgreMatrix4(const aiMatrix4x4& aiMat);
        void applyTransformation(const std::string& boneName, const Ogre::Matrix4 &transform);

        Ogre::SkeletonPtr skeleton;
        std::map<std::string, aiBone*> aiBonesMap;
};
