#pragma once

#include <Ogre.h>
#include <assimp/scene.h>

#include <QString>

#include <cstddef>

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
    // Encoded image bytes (PNG/JPEG/... or WebP) -> a texture named `resourceName`.
    // WebP is decoded here (libwebp): Ogre's stb_image codec cannot read it.
    static Ogre::TexturePtr loadTextureFromBytes(const Ogre::String& resourceName,
                                                 const void* data, std::size_t size,
                                                 const Ogre::String& formatHint);
    void applyRTSSNormalMap(Ogre::MaterialPtr mat, const Ogre::String& normalMapName);

    QString m_sourceDirectory;
    std::vector<Ogre::MaterialPtr> materials;
};
