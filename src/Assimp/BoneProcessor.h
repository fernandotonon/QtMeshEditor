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

        // Generalized root-pose rotation bake (#933): rotate every ROOT bone's
        // rest pose by `rotation`. Same sequencing contract as bakeZupToYup.
        // Used to bake a Blender-style node-chain orientation (Y-up metadata
        // with the standing rotation on the armature/mesh NODES) so the bind
        // pose renders upright.
        static void bakeRootRotation(const Ogre::SkeletonPtr& skeleton,
                                     const Ogre::Quaternion& rotation);

        // World (scene-root-relative) transform of a node: product of
        // mTransformation from the scene root down to `node`. Public so the
        // Importer can read the skinned mesh node's orientation (#933).
        static Ogre::Matrix4 nodeWorldTransform(const aiNode* node);
        // Node that references mesh index `meshIndex` (nullptr if none).
        static const aiNode* findMeshNode(const aiNode* node, unsigned meshIndex);

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
