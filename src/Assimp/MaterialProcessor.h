#pragma once

#include <Ogre.h>
#include <assimp/scene.h>

class MaterialProcessor
{
public:
    void loadScene(const aiScene* scene);
    Ogre::MaterialPtr operator[](unsigned int index);
    unsigned int size();
    
private:
    Ogre::MaterialPtr processMaterial(aiMaterial* material);
    std::vector<Ogre::MaterialPtr> materials;
};
