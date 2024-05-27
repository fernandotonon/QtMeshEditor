#include "MaterialProcessor.h"

void MaterialProcessor::loadScene(const aiScene* scene)
{
    for(auto i = 0u; i < scene->mNumMaterials; i++) {
        aiMaterial* material = scene->mMaterials[i];
        Ogre::MaterialPtr ogreMaterial = processMaterial(material, scene);
        materials.push_back(ogreMaterial);
    }
}
Ogre::MaterialPtr MaterialProcessor::operator[](unsigned int index)
{
    return materials[index];
}

unsigned long MaterialProcessor::size() const
{
    return materials.size();
}

Ogre::MaterialPtr MaterialProcessor::processMaterial(aiMaterial *material, const aiScene* scene)
{
    std::string materialName = material->GetName().C_Str();
    if(materialName.empty()) materialName="importedMaterial" + std::to_string(materials.size());
    if(auto existingMaterial = Ogre::MaterialManager::getSingleton().getByName(materialName))
        return existingMaterial;

    Ogre::MaterialPtr ogreMaterial = Ogre::MaterialManager::getSingleton().create(materialName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    aiColor3D color(0.f, 0.f, 0.f);
    if(AI_SUCCESS == material->Get(AI_MATKEY_COLOR_DIFFUSE, color)) {
        ogreMaterial->getTechnique(0)->getPass(0)->setDiffuse(color.r, color.g, color.b, 1.0f);
    }

    if(AI_SUCCESS == material->Get(AI_MATKEY_COLOR_AMBIENT, color)) {
        ogreMaterial->getTechnique(0)->getPass(0)->setAmbient(color.r, color.g, color.b);
    }

    if(AI_SUCCESS == material->Get(AI_MATKEY_COLOR_SPECULAR, color)) {
        ogreMaterial->getTechnique(0)->getPass(0)->setSpecular(color.r, color.g, color.b, 1.0f);
    }

    if(AI_SUCCESS == material->Get(AI_MATKEY_COLOR_EMISSIVE, color)) {
        ogreMaterial->getTechnique(0)->getPass(0)->setSelfIllumination(color.r, color.g, color.b);
    }

    float shininess = 0.0f;
    if(AI_SUCCESS == material->Get(AI_MATKEY_SHININESS, shininess)) {
        ogreMaterial->getTechnique(0)->getPass(0)->setShininess(shininess);
    }

    // Handle textures
    aiString path;
    if(AI_SUCCESS == material->GetTexture(aiTextureType_DIFFUSE, 0, &path)) {
        std::string texturePath = path.C_Str();
        std::string textureFilename = texturePath.substr(texturePath.find_last_of("/\\") + 1);
        Ogre::TexturePtr texturePtr = Ogre::TextureManager::getSingleton().getByName(textureFilename);

        if(!texturePtr){
            texturePtr = loadTexture(textureFilename, path, scene);
        }
        ogreMaterial->getTechnique(0)->getPass(0)->createTextureUnitState(texturePtr->getName());
    }

    return ogreMaterial;
}

Ogre::TexturePtr MaterialProcessor::loadTexture(const Ogre::String &filename, const aiString &path, const aiScene* scene)
{
    if(auto texture = scene->GetEmbeddedTexture(path.C_Str())) {
        //returned pointer is not null, read texture from memory
        if(texture->mHeight == 0) {
            // The texture data is compressed (e.g., JPEG, PNG, etc.)
            Ogre::DataStreamPtr stream(new Ogre::MemoryDataStream(texture->pcData, texture->mWidth));
            Ogre::Image img;
            img.load(stream, texture->achFormatHint);

            return Ogre::TextureManager::getSingleton().loadImage(
                    filename,
                    Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
                    img
                    );
        } else {
            // The texture data is raw
            Ogre::DataStreamPtr stream(new Ogre::MemoryDataStream(texture->pcData, texture->mWidth * texture->mHeight * 3)); // Assuming RGB 8-bit
            return Ogre::TextureManager::getSingleton().loadRawData(
                filename,
                Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
                stream,
                texture->mWidth,
                texture->mHeight,
                Ogre::PF_R8G8B8  // Assuming RGB 8-bit format
                );
        }
    } 
    //regular file, check if it exists and read it
    return Ogre::TextureManager::getSingleton().load(filename, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
}
