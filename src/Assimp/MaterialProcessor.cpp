#include "MaterialProcessor.h"
#include "RTShaderHelper.h"
#include <OgreRTShaderSystem.h>

namespace {
static Ogre::Pass* ensureFirstPass(const Ogre::MaterialPtr& mat)
{
    if (!mat)
        return nullptr;
    Ogre::Technique* tech = nullptr;
    if (mat->getNumTechniques() == 0)
        tech = mat->createTechnique();
    else
        tech = mat->getTechnique(0);

    if (!tech)
        return nullptr;
    if (tech->getNumPasses() == 0)
        return tech->createPass();
    return tech->getPass(0);
}
} // namespace

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

Ogre::MaterialPtr MaterialProcessor::processMaterial(const aiMaterial *material, const aiScene* scene)
{
    std::string materialName = material->GetName().C_Str();
    if(materialName.empty()) materialName="importedMaterial" + std::to_string(materials.size());

    if(auto existingMaterial = Ogre::MaterialManager::getSingleton().getByName(materialName)) {
        // Material already exists (e.g. from a .material script), but still apply
        // normal maps from Assimp if present, since scripts rarely include RTSS directives.
        aiString existingNormalPath;
        if(AI_SUCCESS == material->GetTexture(aiTextureType_NORMALS, 0, &existingNormalPath)
           || AI_SUCCESS == material->GetTexture(aiTextureType_HEIGHT, 0, &existingNormalPath)) {
            std::string normalTexPath = existingNormalPath.C_Str();
            std::string normalFilename = normalTexPath.substr(normalTexPath.find_last_of("/\\") + 1);
            Ogre::TexturePtr normalTexPtr = Ogre::TextureManager::getSingleton().getByName(normalFilename);
            if(!normalTexPtr) {
                try {
                    normalTexPtr = loadTexture(normalFilename, existingNormalPath, scene);
                } catch (...) {
                    Ogre::LogManager::getSingleton().logMessage("MaterialProcessor: Failed to load normal map '" + normalFilename + "' for existing material '" + materialName + "'");
                }
            }
            if(normalTexPtr) {
                Ogre::LogManager::getSingleton().logMessage("MaterialProcessor: Applying RTSS normal map '" + normalFilename + "' to existing material '" + materialName + "'");
                // Some materials can exist without any techniques/passes (e.g. partially loaded
                // script materials). Ensure a valid pass exists before RTSS touches it.
                (void)ensureFirstPass(existingMaterial);
                applyRTSSNormalMap(existingMaterial, normalTexPtr->getName());
            }
        }
        return existingMaterial;
    }

    Ogre::MaterialPtr ogreMaterial = Ogre::MaterialManager::getSingleton().create(materialName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    Ogre::Pass* pass = ensureFirstPass(ogreMaterial);
    if (!pass)
        return ogreMaterial;

    aiColor3D color(0.f, 0.f, 0.f);
    if(AI_SUCCESS == material->Get(AI_MATKEY_COLOR_DIFFUSE, color)) {
        pass->setDiffuse(color.r, color.g, color.b, 1.0f);
    }

    if(AI_SUCCESS == material->Get(AI_MATKEY_COLOR_AMBIENT, color)) {
        // PBR-workflow exporters often set ambient to (0,0,0) which kills ambient
        // lighting in Ogre's Phong model. Keep Ogre's default (white) in that case.
        if(color.r > 0.001f || color.g > 0.001f || color.b > 0.001f)
            pass->setAmbient(color.r, color.g, color.b);
    }

    if(AI_SUCCESS == material->Get(AI_MATKEY_COLOR_SPECULAR, color)) {
        pass->setSpecular(color.r, color.g, color.b, 1.0f);
    }

    if(AI_SUCCESS == material->Get(AI_MATKEY_COLOR_EMISSIVE, color)) {
        pass->setSelfIllumination(color.r, color.g, color.b);
    }

    float shininess = 0.0f;
    if(AI_SUCCESS == material->Get(AI_MATKEY_SHININESS, shininess)) {
        pass->setShininess(shininess);
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
        auto* tus = pass->createTextureUnitState(texturePtr->getName());
        tus->setName("diffuse_map");
    }

    // Handle normal maps via RTSS (check NORMALS first, then HEIGHT as fallback for Blender exports)
    aiString normalPath;
    bool hasNormalMap = (AI_SUCCESS == material->GetTexture(aiTextureType_NORMALS, 0, &normalPath))
                     || (AI_SUCCESS == material->GetTexture(aiTextureType_HEIGHT, 0, &normalPath));
    if(hasNormalMap) {
        std::string normalTexPath = normalPath.C_Str();
        std::string normalFilename = normalTexPath.substr(normalTexPath.find_last_of("/\\") + 1);
        Ogre::TexturePtr normalTexPtr = Ogre::TextureManager::getSingleton().getByName(normalFilename);
        if(!normalTexPtr) {
            try {
                normalTexPtr = loadTexture(normalFilename, normalPath, scene);
            } catch (...) {
                Ogre::LogManager::getSingleton().logMessage("MaterialProcessor: Failed to load normal map '" + normalFilename + "'");
            }
        }
        if(normalTexPtr)
            applyRTSSNormalMap(ogreMaterial, normalTexPtr->getName());
    }

    // Slice F3: read PBR-specific texture types from Assimp and bind
    // them to the slice E canonical slot names so the user can see
    // them in the Material Editor and they survive
    // export round-trips. We deliberately do NOT auto-promote the
    // material to Cook-Torrance shading here — that path needs IBL to
    // not look dark and is a separate slice. The slots are also tagged
    // with `pbr_workflow=metallic_roughness` so a future "Convert to
    // PBR" inspector action can apply Cook-Torrance to the existing
    // textures rather than the user having to re-bind everything.
    //
    //   aiTextureType_BASE_COLOR        → "albedo"
    //   aiTextureType_METALNESS         → "metallic"  (or packed glTF MR)
    //   aiTextureType_DIFFUSE_ROUGHNESS → "roughness"
    //   aiTextureType_AMBIENT_OCCLUSION → "ao"
    //   aiTextureType_EMISSIVE          → "emissive"
    auto bindPbrSlot = [&](aiTextureType type, const std::string& slotName) {
        aiString p;
        if (material->GetTexture(type, 0, &p) != AI_SUCCESS) return false;
        const std::string sp = p.C_Str();
        const std::string fn = sp.substr(sp.find_last_of("/\\") + 1);
        if (fn.empty()) return false;
        Ogre::TexturePtr tex = Ogre::TextureManager::getSingleton().getByName(fn);
        if (!tex) {
            try { tex = loadTexture(fn, p, scene); }
            catch (...) {
                Ogre::LogManager::getSingleton().logMessage(
                    "MaterialProcessor: Failed to load PBR map '" + fn +
                    "' for slot '" + slotName + "'");
                return false;
            }
        }
        if (!tex) return false;

        auto* tus = pass->createTextureUnitState(tex->getName());
        tus->setName(slotName);
        // Mark non-FFP for everything except albedo. Albedo modulates
        // with the existing diffuse layer naturally; the others would
        // stack as garbage layers and darken the visible surface.
        if (slotName != "albedo") {
            Ogre::RTShader::ShaderGenerator::_markNonFFP(tus);
        }
        return true;
    };

    // If no BASE_COLOR is exposed but a legacy DIFFUSE was bound above,
    // also expose it under the canonical "albedo" slot so PBR tooling
    // (e.g. a future "Convert to PBR" action, or Slice F's Cook-Torrance
    // path) finds it. Many FBX exporters write the base colour under
    // aiTextureType_DIFFUSE only — without this fallback the albedo
    // slot stays empty even though a clearly-albedo texture exists.
    bool hasAlbedoSlot = false;
    {
        aiString p;
        if (material->GetTexture(aiTextureType_BASE_COLOR, 0, &p) == AI_SUCCESS) {
            hasAlbedoSlot = true;
        }
    }

    bool gotPbrMap = false;
    gotPbrMap |= bindPbrSlot(aiTextureType_BASE_COLOR,         "albedo");
    gotPbrMap |= bindPbrSlot(aiTextureType_METALNESS,          "metallic");
    // Probe both DIFFUSE_ROUGHNESS and SHININESS — different exporters
    // (Blender vs. native FBX SDK) use one or the other. UNKNOWN is the
    // catch-all Assimp uses when an FBX texture's role isn't recognised.
    gotPbrMap |= bindPbrSlot(aiTextureType_DIFFUSE_ROUGHNESS,  "roughness");
    if (!gotPbrMap || !pass->getTextureUnitState("roughness")) {
        bindPbrSlot(aiTextureType_SHININESS, "roughness");
    }
    gotPbrMap |= bindPbrSlot(aiTextureType_AMBIENT_OCCLUSION,  "ao");
    gotPbrMap |= bindPbrSlot(aiTextureType_EMISSIVE,           "emissive");

    // Fallback: if no BASE_COLOR was found but a legacy diffuse_map
    // exists (created by the DIFFUSE branch above), reuse its texture
    // for the albedo slot. We don't create a duplicate TUS — instead
    // we add a second alias slot pointing at the same texture, so the
    // existing FFP texturing chain still works. This is what most
    // PBR-aware DCCs do when round-tripping FBX↔glTF.
    if (!hasAlbedoSlot && gotPbrMap) {
        for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
            auto* tus = pass->getTextureUnitState(i);
            if (tus->getName() == "diffuse_map" && !tus->getTextureName().empty()) {
                auto* alb = pass->createTextureUnitState(tus->getTextureName());
                alb->setName("albedo");
                Ogre::RTShader::ShaderGenerator::_markNonFFP(alb);
                break;
            }
        }
    }

    // NOTE: We deliberately do NOT tag the pass with `pbr_workflow` on
    // import. Tagging would trigger applyPbrIfTagged via the slice F2
    // applyNormalMap redirect, attaching SRS_COOK_TORRANCE_LIGHTING.
    // Without IBL, Cook-Torrance produces near-black output for
    // metallic surfaces (the diffuse term is baseColor × (1 - metallic)
    // and there's no env map to supply indirect specular). A future
    // slice with proper IBL can either tag-on-import then or expose a
    // "Convert to PBR" inspector action that adds the tag deliberately.
    // For now: slots are populated and visible in the Material Editor,
    // and the rendered material continues using the legacy FFP diffuse
    // path (correct on-import visuals).
    (void)gotPbrMap;

    return ogreMaterial;
}

Ogre::TexturePtr MaterialProcessor::loadTexture(const Ogre::String &filename, const aiString &path, const aiScene* scene) const
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

void MaterialProcessor::applyRTSSNormalMap(Ogre::MaterialPtr mat, const Ogre::String& normalMapName)
{
    RTShaderHelper::applyNormalMap(mat, normalMapName);
}
