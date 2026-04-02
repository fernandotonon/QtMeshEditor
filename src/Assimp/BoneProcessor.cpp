#include "BoneProcessor.h"
#include <set>

void BoneProcessor::processBones(Ogre::SkeletonPtr skeleton, const aiScene *scene, bool isZup) {
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
                Ogre::Matrix4 rootBoneGlobalTransformation = convertToOgreMatrix4(bone->mNode->mParent->mTransformation);
                applyTransformation(bone->mNode->mParent->mName.C_Str(), rootBoneGlobalTransformation);
            }
        }
    }

    // Start from the root node and process the hierarchy
    processBoneHierarchy(scene->mRootNode);

    // For animation-only files (no mesh bones), create bones from animation channels.
    if (skeleton->getNumBones() == 0 && scene->HasAnimations()) {
        // Collect directly animated node names.
        // Do NOT walk up to include non-animated ancestors — scene grouping nodes
        // (e.g. Unreal's "Armature") must not become bones as they would break
        // the hierarchy when merging into a mesh skeleton that treats the
        // skeleton root (e.g. "root") as a top-level bone.
        std::set<std::string> animatedNodes;
        for (unsigned i = 0; i < scene->mNumAnimations; ++i)
            for (unsigned j = 0; j < scene->mAnimations[i]->mNumChannels; ++j)
                animatedNodes.insert(scene->mAnimations[i]->mChannels[j]->mNodeName.C_Str());

        processAnimationOnlyHierarchy(scene->mRootNode, animatedNodes);
    }

    // Bake Z-up → Y-up into root bone rest poses so no scene-node rotation is needed.
    // Only root bones (no parent in the Ogre skeleton) need to be rotated; child bones'
    // local transforms are relative to their parent and are correct as-is.
    if (isZup) {
        const Ogre::Quaternion R_x90(Ogre::Degree(90), Ogre::Vector3::UNIT_X);
        for (unsigned short i = 0; i < skeleton->getNumBones(); ++i) {
            Ogre::Bone* bone = skeleton->getBone(i);
            if (bone->getParent() == nullptr) {
                bone->setPosition(R_x90 * bone->getPosition());
                bone->setOrientation(R_x90 * bone->getOrientation());
            }
        }
    }
}

void BoneProcessor::processBoneHierarchy(aiNode* node) {
    bool isSkinned = (aiBonesMap.find(node->mName.C_Str()) != aiBonesMap.end());
    bool isExistingBone = skeleton->hasBone(node->mName.C_Str());

    if (isSkinned) {
        aiBone* bone = aiBonesMap[node->mName.C_Str()];
        createBone(bone->mName.C_Str());
        processBoneNode(bone);
    }

    // Recursively process children
    for (auto i = 0u; i < node->mNumChildren; i++) {
        aiNode* child = node->mChildren[i];

        // If this node is a bone (skinned or already created as root),
        // also create non-skinned children as bones (e.g. leaf/tip bones
        // that have no vertex weights but are part of the skeleton).
        if ((isSkinned || isExistingBone) && child->mNumMeshes == 0 &&
            aiBonesMap.find(child->mName.C_Str()) == aiBonesMap.end() &&
            !skeleton->hasBone(child->mName.C_Str()))
        {
            processNonSkinnedBone(child);
        }

        processBoneHierarchy(child);
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

void BoneProcessor::processNonSkinnedBone(aiNode* node) {
    std::string boneName = node->mName.C_Str();
    createBone(boneName);

    // Use the node's local transform directly.
    Ogre::Matrix4 transform = convertToOgreMatrix4(node->mTransformation);
    applyTransformation(boneName, transform);

    // Set parent-child relationship
    if (node->mParent && node->mParent->mName.length &&
        skeleton->hasBone(node->mParent->mName.C_Str()))
    {
        Ogre::Bone* parentBone = skeleton->getBone(node->mParent->mName.C_Str());
        Ogre::Bone* ogreBone = skeleton->getBone(boneName);
        if (!std::any_of(parentBone->getChildren().begin(), parentBone->getChildren().end(),
                         [&ogreBone](const auto& childNode) {
                             return childNode->getName() == ogreBone->getName();
                         })) {
            parentBone->addChild(ogreBone);
        }
    }
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

void BoneProcessor::processAnimationOnlyHierarchy(aiNode* node, const std::set<std::string>& animatedNodes)
{
    bool isAnimated = animatedNodes.count(node->mName.C_Str()) > 0;
    bool isExistingBone = skeleton->hasBone(node->mName.C_Str());

    if (isAnimated && !isExistingBone) {
        processNonSkinnedBone(node);
        isExistingBone = true;
    }

    for (unsigned i = 0; i < node->mNumChildren; ++i) {
        aiNode* child = node->mChildren[i];
        // Also pull in non-animated children of bone nodes (leaf/tip bones)
        if (isExistingBone && child->mNumMeshes == 0 &&
            !skeleton->hasBone(child->mName.C_Str()))
        {
            processNonSkinnedBone(child);
        }
        processAnimationOnlyHierarchy(child, animatedNodes);
    }
}
