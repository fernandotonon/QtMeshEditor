#pragma once

#include <Ogre.h>
#include <assimp/scene.h>

class BoneProcessor {
    public:
        void processBones(Ogre::SkeletonPtr skeleton, const aiScene* scene);

        // Bake Z-up → Y-up into root bone rest poses.
        // Call this AFTER processAnimations() so animation deltas are computed
        // against the original (pre-bake) T-pose — avoids a basis mismatch for
        // embedded animations in Z-up mesh FBX files.
        static void bakeZupToYup(const Ogre::SkeletonPtr& skeleton);

    private:
        void createBone(const std::string& boneName);
        void processBoneHierarchy(aiNode* node);
        void processBoneNode(aiBone *bone);
        void processNonSkinnedBone(aiNode* node);
        void processAnimationOnlyHierarchy(aiNode* node, const std::set<std::string>& animatedNodes);
        // World (scene-root-relative) transform of a node: product of
        // mTransformation from the scene root down to `node`.
        Ogre::Matrix4 nodeWorldTransform(const aiNode* node) const;
        // Node that references mesh index `meshIndex` (nullptr if none).
        static const aiNode* findMeshNode(const aiNode* node, unsigned meshIndex);
        Ogre::Matrix4 convertToOgreMatrix4(const aiMatrix4x4& aiMat);
        void applyTransformation(const std::string& boneName, const Ogre::Matrix4 &transform);

        Ogre::SkeletonPtr skeleton;
        std::map<std::string, aiBone*> aiBonesMap;
};
