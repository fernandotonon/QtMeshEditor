#include "AnimationProcessor.h"

AnimationProcessor::AnimationProcessor(Ogre::SkeletonPtr skeleton): skeleton(skeleton) {}

void AnimationProcessor::processAnimations(const aiScene* scene) {
    for(auto i = 0u; i < scene->mNumAnimations; i++) {
        aiAnimation* animation = scene->mAnimations[i];
        processAnimation(animation, scene);
    }
}

void AnimationProcessor::processAnimation(aiAnimation* animation, const aiScene* scene) {
    // get the animation speed
    auto mTicksPerSecond = (Ogre::Real)((0 == animation->mTicksPerSecond) ? 24.0f : animation->mTicksPerSecond);
    // Create the animation
    Ogre::Animation* ogreAnimation = skeleton->createAnimation(animation->mName.C_Str(), animation->mDuration/mTicksPerSecond);
    // Process the animation channels
    for(auto i = 0u; i < animation->mNumChannels; i++) {
        aiNodeAnim* nodeAnim = animation->mChannels[i];
        processAnimationChannel(nodeAnim, ogreAnimation, scene, i, mTicksPerSecond);
    }
}

void AnimationProcessor::processAnimationChannel(aiNodeAnim* nodeAnim, Ogre::Animation* animation, const aiScene* scene, unsigned int channelIndex, Ogre::Real mTicksPerSecond) {
    if(!skeleton->hasBone(nodeAnim->mNodeName.C_Str())) return;

    // Create the animation track
    Ogre::Bone* bone = skeleton->getBone(nodeAnim->mNodeName.C_Str());
    Ogre::NodeAnimationTrack* track = animation->createNodeTrack(bone->getHandle(), bone);

    // Create a map to store keyframes by time
    std::map<double, std::tuple<Ogre::Vector3, Ogre::Quaternion, Ogre::Vector3>> keyframes;

    // Get the bone's T-pose position
    auto boneTPosePosition = bone->getPosition();
    // Process the position keys
    for(auto i = 0u; i < nodeAnim->mNumPositionKeys; i++) {
        aiVectorKey positionKey = nodeAnim->mPositionKeys[i];
        Ogre::Vector3 position(positionKey.mValue.x, positionKey.mValue.y, positionKey.mValue.z);
        // Convert the position from local space to model space
        position = position - boneTPosePosition;

        keyframes[positionKey.mTime] = std::make_tuple(
            position,
            Ogre::Quaternion::IDENTITY,
            Ogre::Vector3::UNIT_SCALE
            );
    }

    // Process the rotation keys
    Ogre::Quaternion boneTPoseInverseRotation = bone->getOrientation().Inverse();
    for(auto i = 0u; i < nodeAnim->mNumRotationKeys; i++) {
        aiQuatKey rotationKey = nodeAnim->mRotationKeys[i];
        Ogre::Quaternion rot(rotationKey.mValue.w, rotationKey.mValue.x, rotationKey.mValue.y, rotationKey.mValue.z);
        rot = boneTPoseInverseRotation * rot; // Convert from local space to model space
        rot.normalise(); // Normalize the quaternion
        if (keyframes.find(rotationKey.mTime) == keyframes.end()) {
            keyframes[rotationKey.mTime] = std::make_tuple(
                Ogre::Vector3::ZERO,
                rot,
                Ogre::Vector3::UNIT_SCALE
                );
        } else {
            std::get<1>(keyframes[rotationKey.mTime]) = rot;
        }
    }

    // Process the scaling keys
    for(auto i = 0u; i < nodeAnim->mNumScalingKeys; i++) {
        aiVectorKey scalingKey = nodeAnim->mScalingKeys[i];
        Ogre::Vector3 scale(scalingKey.mValue.x, scalingKey.mValue.y, scalingKey.mValue.z);
        if (keyframes.find(scalingKey.mTime) == keyframes.end()) {
            keyframes[scalingKey.mTime] = std::make_tuple(
                Ogre::Vector3::ZERO,
                Ogre::Quaternion::IDENTITY,
                scale
                );
        } else {
            std::get<2>(keyframes[scalingKey.mTime]) = scale;
        }
    }

    // Now create the keyframes in the track
    for(auto& [time, transform] : keyframes) {
        Ogre::TransformKeyFrame* keyFrame = track->createNodeKeyFrame(time/mTicksPerSecond);
        keyFrame->setTranslate(std::get<0>(transform));
        keyFrame->setRotation(std::get<1>(transform));
        keyFrame->setScale(std::get<2>(transform));
    }
}
