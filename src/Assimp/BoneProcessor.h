#pragma once

#include <Ogre.h>
#include <assimp/scene.h>

class BoneProcessor {
    public:
        void processBones(Ogre::SkeletonPtr skeleton, const aiScene* scene);

    private:
        void createBone(const std::string& boneName);
        void processBoneHierarchy(aiBone* bone);
        void processBoneNode(aiBone *bone);

        Ogre::SkeletonPtr skeleton;    
        std::map<std::string, aiBone*> aiBonesMap;
};
