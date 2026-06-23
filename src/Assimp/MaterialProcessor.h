#pragma once

#include <Ogre.h>
#include <assimp/scene.h>

#include <QString>

class MaterialProcessor
{
public:
    void setSourceDirectory(const QString& absoluteDir) { m_sourceDirectory = absoluteDir; }

    void loadScene(const aiScene* scene);
    Ogre::MaterialPtr operator[](unsigned int index);
    unsigned long size() const;

private:
    Ogre::MaterialPtr processMaterial(const aiMaterial* material, const aiScene* scene);
    Ogre::TexturePtr loadTexture(const Ogre::String& filename, const aiString& path, const aiScene* scene);
    Ogre::TexturePtr resolveTexture(const std::string& filename, const aiString& path, const aiScene* scene);
    Ogre::TexturePtr loadTextureFromFile(const QString& absolutePath, const Ogre::String& resourceName);
    void applyRTSSNormalMap(Ogre::MaterialPtr mat, const Ogre::String& normalMapName);

    QString m_sourceDirectory;
    std::vector<Ogre::MaterialPtr> materials;
};
