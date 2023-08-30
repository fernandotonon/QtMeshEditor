#include "BoneProcessor.h"

void BoneProcessor::processBones(Ogre::SkeletonPtr skeleton, const aiScene *scene) {
    this->skeleton = skeleton;

    // First, create a map of bone names to aiBones for easier look-up
    for(auto i = 0u; i < scene->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[i];
        for(auto j = 0u; j < mesh->mNumBones; j++) {
            aiBone* bone = mesh->mBones[j];
            aiBonesMap[bone->mName.C_Str()] = bone;
        }
    }

    // Create the root bones
    for(auto i = 0u; i < scene->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[i];
        for(auto j = 0u; j < mesh->mNumBones; j++) {
            aiBone* bone = mesh->mBones[j];
            if(bone->mNode && bone->mNode->mParent && !skeleton->hasBone(bone->mNode->mParent->mName.C_Str())) {
                createBone(bone->mNode->mParent->mName.C_Str());
                Ogre::Matrix4 rootBoneGlobalTransformation = convertToOgreMatrix4(bone->mNode->mParent->mTransformation).inverse();
                applyTransformation(bone->mNode->mParent->mName.C_Str(), rootBoneGlobalTransformation);
            }
        }
    }

    // Start from the root node and process the hierarchy
    processBoneHierarchy(scene->mRootNode);
}

void BoneProcessor::processBoneHierarchy(aiNode* node) {
    // Check if this node corresponds to a bone
    if(aiBonesMap.find(node->mName.C_Str()) != aiBonesMap.end()) {
        aiBone* bone = aiBonesMap[node->mName.C_Str()];
        createBone(bone->mName.C_Str());
        processBoneNode(bone);

        // Recursively process children bones
        for(auto i = 0u; i < node->mNumChildren; i++) {
            aiNode* childNode = node->mChildren[i];
            processBoneHierarchy(childNode);
        }
    }
    else {
        // If this node isn't a bone, still process its children
        for(auto i = 0u; i < node->mNumChildren; i++) {
            aiNode* childNode = node->mChildren[i];
            processBoneHierarchy(childNode);
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

Ogre::Matrix4 BoneProcessor::convertToOgreMatrix4(const aiMatrix4x4& aiMat) {
    return Ogre::Matrix4(
        aiMat.a1, aiMat.a2, aiMat.a3, aiMat.a4,
        aiMat.b1, aiMat.b2, aiMat.b3, aiMat.b4,
        aiMat.c1, aiMat.c2, aiMat.c3, aiMat.c4,
        aiMat.d1, aiMat.d2, aiMat.d3, aiMat.d4
        );
}

void BoneProcessor::applyTransformation(const std::string &boneName, const Ogre::Matrix4 &transform)
{
    // Convert the Ogre::Matrix4 to an Ogre::Affine3
    Ogre::Affine3 affine(transform);

    // Decompose the offset matrix into position, scale, and orientation
    Ogre::Vector3 position, scale;
    Ogre::Quaternion orientation;
    affine.decomposition(position, scale, orientation);

    // Retrieve the bone (it should already exist)
    Ogre::Bone* ogreBone = skeleton->getBone(boneName);

    // Set the bone's position, orientation, and scale
    ogreBone->setPosition(position);
    ogreBone->setOrientation(orientation);
    ogreBone->setScale(scale);
}

void BoneProcessor::processBoneNode(aiBone* bone) {
    // Convert the aiBone's offset matrix to an Ogre::Matrix4
    Ogre::Matrix4 offsetMatrix = convertToOgreMatrix4(bone->mOffsetMatrix);

    // Invert the offset matrix to get the global transformation of the bone
    Ogre::Matrix4 globalTransform = offsetMatrix.inverse();

    // If the bone has a parent, multiply the global transformation of the bone with the inverse of the global transformation of the parent to get the local transformation
    if(bone->mNode->mParent && bone->mNode->mParent->mName.length) {
        if(skeleton->hasBone(bone->mNode->mParent->mName.C_Str())){
            Ogre::Bone* parentBone = skeleton->getBone(bone->mNode->mParent->mName.C_Str());
            Ogre::Matrix4 parentGlobalTransform = parentBone->_getFullTransform().inverse();
            globalTransform = parentGlobalTransform * globalTransform;
        }
    }

    applyTransformation(bone->mName.C_Str(), globalTransform);

    // Add the bone to the parent bone, if it exists
    if(bone->mNode->mParent && bone->mNode->mParent->mName.length) {
        if(skeleton->hasBone(bone->mNode->mParent->mName.C_Str())){
            Ogre::Bone* parentBone = skeleton->getBone(bone->mNode->mParent->mName.C_Str());
            Ogre::Bone* ogreBone = skeleton->getBone(bone->mName.C_Str());
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
