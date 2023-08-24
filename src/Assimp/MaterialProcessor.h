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
    Ogre::MaterialPtr processMaterial(aiMaterial* material, const aiScene* scene);
    Ogre::TexturePtr loadTexture(const Ogre::String& filename, const aiString& path, const aiScene* scene);

    std::vector<Ogre::MaterialPtr> materials;
};
