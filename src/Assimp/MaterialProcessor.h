#pragma once

#include <Ogre.h>
#include <assimp/scene.h>

class MaterialProcessor
{
public:
    void loadScene(const aiScene* scene);
    Ogre::MaterialPtr operator[](unsigned int index);
    unsigned long size() const;

private:
    Ogre::MaterialPtr processMaterial(const aiMaterial* material, const aiScene* scene);
    Ogre::TexturePtr loadTexture(const Ogre::String& filename, const aiString& path, const aiScene* scene) const;

    std::vector<Ogre::MaterialPtr> materials;
};
