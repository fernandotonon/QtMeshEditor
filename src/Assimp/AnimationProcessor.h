#pragma once

#include <Ogre.h>
#include <assimp/scene.h>

class AnimationProcessor
{
public:
    AnimationProcessor(Ogre::SkeletonPtr skeleton): skeleton(skeleton) {}
    void processAnimations(const aiScene* scene);

private:
    void processAnimation(aiAnimation* animation, const aiScene* scene);
    void processAnimationChannel(aiNodeAnim* nodeAnim, Ogre::Animation* animation, const aiScene* scene, unsigned int channelIndex);
    Ogre::SkeletonPtr skeleton;
};

