#pragma once

#include <Ogre.h>
#include <assimp/scene.h>

class MaterialProcessor
{
public:
    void loadScene(const aiScene* scene);
    Ogre::MaterialPtr operator[](unsigned int index);
    unsigned int size();

protected:
    Ogre::MaterialPtr processMaterial(aiMaterial* material);

    // For testing purposes
    friend class MaterialProcessorTest;    
private:
    std::vector<Ogre::MaterialPtr> materials;
};
