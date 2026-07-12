#pragma once

#include <Ogre.h>
#include <assimp/scene.h>

class AnimationProcessor
{
public:
    AnimationProcessor(Ogre::SkeletonPtr skeleton);
    void processAnimations(const aiScene* scene);

    // Consume Assimp's morph-weight channels (aiAnimation::mMorphMeshChannels,
    // populated by the glTF2 importer) and build mesh-level VAT_POSE weight
    // clips on the already-built Ogre mesh — the SAME structure
    // MorphAnimationManager authors, so the dope sheet / Animation list /
    // playback treat imported and authored morph clips identically.
    //
    // MUST be called AFTER MeshProcessor::createMesh(): the morph poses it
    // references only exist once the mesh + poses have been built.
    // No-op when the scene has no morph channels or the mesh has no poses.
    static void processMorphWeightAnimations(const Ogre::MeshPtr& mesh, const aiScene* scene);

private:
    void processAnimation(aiAnimation* animation, const aiScene* scene);
    void processAnimationChannel(aiNodeAnim* nodeAnim, Ogre::Animation* animation, const aiScene* scene, unsigned int channelIndex, Ogre::Real mTicksPerSecond);
    Ogre::SkeletonPtr skeleton;
};

