#include "BoneProcessor.h"

void BoneProcessor::processBoneHierarchy(aiBone* bone) {
    // Process the bone node
    processBoneNode(bone);

    // Recursively process children bones
    for(auto i = 0u; i < bone->mNode->mNumChildren; i++) {
        aiNode* childNode = bone->mNode->mChildren[i];
        if(aiBonesMap.find(childNode->mName.C_Str()) != aiBonesMap.end()) {
            processBoneHierarchy(aiBonesMap[childNode->mName.C_Str()]);
        }
    }
}

void BoneProcessor::processBones(Ogre::SkeletonPtr skeleton, const aiScene *scene) {
    this->skeleton = skeleton;
    for(auto i = 0u; i < scene->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[i];
        for(auto j = 0u; j < mesh->mNumBones; j++) {
            aiBone* bone = mesh->mBones[j];
            // Check if the bone already exists
            if(!skeleton->hasBone(bone->mName.C_Str())) {
                aiBonesMap[bone->mName.C_Str()] = bone;
                createBone(bone->mName.C_Str());
            }
        }
    }

    // Some models have bone animations but not all the bones are related to the meshes
    for(auto i = 0u; i < scene->mNumAnimations; i++) {
        aiAnimation* anim = scene->mAnimations[i];
        for(auto j = 0u; j < anim->mNumChannels; j++) {
            aiNodeAnim* nodeAnim = anim->mChannels[j];
            createBone(nodeAnim->mNodeName.C_Str());
        }
    }

    // Process the root bones first
    for(auto i = 0u; i < scene->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[i];
        for(auto j = 0u; j < mesh->mNumBones; j++) {
            aiBone* bone = mesh->mBones[j];
            if(!skeleton->hasBone(bone->mNode->mParent->mName.C_Str())) {
                createBone(bone->mNode->mParent->mName.C_Str());
                processBoneHierarchy(bone);
            }
        }
    }

}

void BoneProcessor::createBone(const std::string& boneName) {
    // Check if the bone already exists
    if(!skeleton->hasBone(boneName)) {
        // If the bone does not exist, create it
        skeleton->createBone(boneName);
    }
}

void BoneProcessor::processBoneNode(aiBone* bone) {
    // Convert the aiBone's offset matrix to an Ogre::Matrix4
    Ogre::Matrix4 offsetMatrix(
        bone->mOffsetMatrix.a1, bone->mOffsetMatrix.a2, bone->mOffsetMatrix.a3, bone->mOffsetMatrix.a4,
        bone->mOffsetMatrix.b1, bone->mOffsetMatrix.b2, bone->mOffsetMatrix.b3, bone->mOffsetMatrix.b4,
        bone->mOffsetMatrix.c1, bone->mOffsetMatrix.c2, bone->mOffsetMatrix.c3, bone->mOffsetMatrix.c4,
        bone->mOffsetMatrix.d1, bone->mOffsetMatrix.d2, bone->mOffsetMatrix.d3, bone->mOffsetMatrix.d4
        );

    // Invert the offset matrix to get the global transformation of the bone
    Ogre::Matrix4 globalTransform = offsetMatrix.inverse();

    // If the bone has a parent, multiply the global transformation of the bone with the inverse of the global transformation of the parent to get the local transformation
    if(bone->mNode->mParent && bone->mNode->mParent->mName.length) {
        Ogre::Bone* parentBone = skeleton->getBone(bone->mNode->mParent->mName.C_Str());
        if(parentBone) {
            Ogre::Matrix4 parentGlobalTransform = parentBone->_getFullTransform().inverse();
            globalTransform = parentGlobalTransform * globalTransform;
        }
    }

    // Convert the Ogre::Matrix4 to an Ogre::Affine3
    Ogre::Affine3 affine(globalTransform);

    // Decompose the offset matrix into position, scale, and orientation
    Ogre::Vector3 position, scale;
    Ogre::Quaternion orientation;
    affine.decomposition(position, scale, orientation);

    // Retrieve the bone (it should already exist)
    Ogre::Bone* ogreBone = skeleton->getBone(bone->mName.C_Str());

    // Set the bone's position, orientation, and scale
    ogreBone->setPosition(position);
    ogreBone->setOrientation(orientation);
    ogreBone->setScale(scale);

    // Add the bone to the parent bone, if it exists
    if(bone->mNode->mParent && bone->mNode->mParent->mName.length) {
        Ogre::Bone* parentBone = skeleton->getBone(bone->mNode->mParent->mName.C_Str());
        if(parentBone) {
            // Check if ogreBone is already a child of parentBone
            if (!std::any_of(parentBone->getChildren().begin(), parentBone->getChildren().end(),
                             [&ogreBone](const auto& childNode) {
                                 return childNode->getName() == ogreBone->getName();
                             })) {
                parentBone->addChild(ogreBone);
            }
        }
    }
}