#include "MaterialProcessor.h"
#include "RTShaderHelper.h"
#include "EmbeddedTextureCache.h"

#include <cstddef>

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
        // Material already exists (e.g. from a .material script, or from
        // a previous import in the same session). Apply Assimp data on
        // top — but only ADD what the existing material is missing,
        // never replace TUS that are already there. Scripts win for slots
        // they explicitly define, and Assimp augments with the rest.
        Ogre::Pass* xPass = ensureFirstPass(existingMaterial);
        if (!xPass) return existingMaterial;

        Ogre::String stagedNormalTex;
        auto stageNormalFromAssimp = [&](aiTextureType type) -> bool {
            aiString path;
            if (material->GetTexture(type, 0, &path) != AI_SUCCESS)
                return false;
            const std::string texPath = path.C_Str();
            const std::string filename =
                texPath.substr(texPath.find_last_of("/\\") + 1);
            if (filename.empty())
                return false;
            Ogre::TexturePtr tex = Ogre::TextureManager::getSingleton().getByName(filename);
            if (!tex) {
                try {
                    tex = loadTexture(filename, path, scene);
                } catch (...) {
                    return false;
                }
            }
            if (!tex)
                return false;
            stagedNormalTex = tex->getName();
            return true;
        };
        if (!stageNormalFromAssimp(aiTextureType_NORMALS))
            stageNormalFromAssimp(aiTextureType_NORMAL_CAMERA);
        if (stagedNormalTex.empty())
            stageNormalFromAssimp(aiTextureType_HEIGHT);

        // PBR slots: add any that are missing on the existing material.
        // Without this, reimporting an FBX whose material already exists
        // (e.g. from a previous import in this session) would leave the
        // PBR slots stale — pointing at the texture pointers from the
        // first import — and the embedded reimport textures would never
        // bind, producing the user-visible "different from original"
        // shading on round-tripped FBXes.
        auto addMissingSlot = [&](aiTextureType type, const std::string& slotName) {
            if (xPass->getTextureUnitState(slotName)) return;
            aiString p;
            if (material->GetTexture(type, 0, &p) != AI_SUCCESS) return;
            std::string sp = p.C_Str();
            std::string fn = sp.substr(sp.find_last_of("/\\") + 1);
            if (fn.empty()) return;
            Ogre::TexturePtr tex = Ogre::TextureManager::getSingleton().getByName(fn);
            if (!tex) {
                try { tex = loadTexture(fn, p, scene); }
                catch (...) { return; }
            }
            if (!tex) return;
            auto* tus = xPass->createTextureUnitState(tex->getName());
            tus->setName(slotName);
            if (slotName != "albedo"
                && Ogre::RTShader::ShaderGenerator::getSingletonPtr()) {
                Ogre::RTShader::ShaderGenerator::_markNonFFP(tus);
            }
        };
        addMissingSlot(aiTextureType_METALNESS,          "metallic");
        addMissingSlot(aiTextureType_DIFFUSE_ROUGHNESS,  "roughness");
        if (!xPass->getTextureUnitState("roughness"))
            addMissingSlot(aiTextureType_SHININESS,      "roughness");
        addMissingSlot(aiTextureType_AMBIENT_OCCLUSION,  "ao");
        addMissingSlot(aiTextureType_EMISSIVE,           "emissive");
        if (!xPass->getTextureUnitState("emissive"))
            addMissingSlot(aiTextureType_EMISSION_COLOR, "emissive");
        addMissingSlot(aiTextureType_BASE_COLOR,         "albedo");

        if (Ogre::Root::getSingletonPtr() && Ogre::Root::getSingletonPtr()->getRenderSystem()) {
            RTShaderHelper::finalizeShaderGenMaterial(existingMaterial, stagedNormalTex);
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

    // Handle normal maps via RTSS. Probe in order:
    //   NORMALS         — generic "NormalMap" property (most exporters)
    //   HEIGHT          — Blender's older bump/normal output
    //   NORMAL_CAMERA   — Assimp routes Maya|TEX_normal_map here on FBX
    //                     reimport, so this lets a Maya-Stingray-styled
    //                     FBX round-trip its normal map back into our
    //                     RTSS pipeline.
    Ogre::String stagedNormalTex;
    auto stageNormalFromAssimp = [&](aiTextureType type) -> bool {
        aiString path;
        if (material->GetTexture(type, 0, &path) != AI_SUCCESS)
            return false;
        const std::string texPath = path.C_Str();
        const std::string filename = texPath.substr(texPath.find_last_of("/\\") + 1);
        if (filename.empty())
            return false;
        Ogre::TexturePtr tex = Ogre::TextureManager::getSingleton().getByName(filename);
        if (!tex) {
            try {
                tex = loadTexture(filename, path, scene);
            } catch (...) {
                return false;
            }
        }
        if (!tex)
            return false;
        stagedNormalTex = tex->getName();
        return true;
    };
    if (!stageNormalFromAssimp(aiTextureType_NORMALS))
        stageNormalFromAssimp(aiTextureType_NORMAL_CAMERA);
    if (stagedNormalTex.empty())
        stageNormalFromAssimp(aiTextureType_HEIGHT);

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
        // Guard the call: in unit-test fixtures only MaterialManager is
        // initialised — Ogre::RTShader::ShaderGenerator hasn't been set
        // up, and _markNonFFP segfaults on the missing singleton there.
        if (slotName != "albedo"
            && Ogre::RTShader::ShaderGenerator::getSingletonPtr()) {
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

    // Order matters: we want the final TUS layout to match what a typical
    // first-import of a third-party PBR FBX produces, which is
    // [diffuse_map, normal_map(RTSS), metallic, roughness, ao, emissive, albedo].
    // Most third-party exporters use ReflectionFactor / ShininessExponent
    // (no Maya|TEX_color_map), so on first-import BASE_COLOR is missing and
    // `albedo` is created last via the diffuse_map alias fallback. To make
    // re-imports of our own export look identical, we add the BASE_COLOR
    // albedo slot at the very end too.
    bool gotPbrMap = false;
    gotPbrMap |= bindPbrSlot(aiTextureType_METALNESS,          "metallic");
    // Probe both DIFFUSE_ROUGHNESS and SHININESS — different exporters
    // (Blender vs. native FBX SDK, vs. Maya Stingray) use one or the other.
    gotPbrMap |= bindPbrSlot(aiTextureType_DIFFUSE_ROUGHNESS,  "roughness");
    if (!pass->getTextureUnitState("roughness")) {
        gotPbrMap |= bindPbrSlot(aiTextureType_SHININESS, "roughness");
    }
    gotPbrMap |= bindPbrSlot(aiTextureType_AMBIENT_OCCLUSION,  "ao");
    // EMISSIVE vs. EMISSION_COLOR: Assimp's FBXConverter routes
    // Maya|TEX_emissive_map to aiTextureType_EMISSION_COLOR (not EMISSIVE).
    gotPbrMap |= bindPbrSlot(aiTextureType_EMISSIVE,           "emissive");
    if (!pass->getTextureUnitState("emissive"))
        gotPbrMap |= bindPbrSlot(aiTextureType_EMISSION_COLOR, "emissive");

    // Albedo last — this matches the slot layout produced when a FBX
    // exposes only DiffuseColor (no Maya|TEX_color_map): the legacy
    // DIFFUSE branch creates `diffuse_map` first, then we synthesise an
    // `albedo` alias at the end. Doing the same for FBXes that DO carry
    // BASE_COLOR keeps the post-import material identical regardless of
    // how the file was authored.
    gotPbrMap |= bindPbrSlot(aiTextureType_BASE_COLOR,         "albedo");

    // Fallback: if no BASE_COLOR was exposed but a legacy diffuse_map was
    // created above, alias diffuse_map's texture into the canonical
    // `albedo` slot so PBR-aware tooling (templates, Cook-Torrance) finds
    // it. Most third-party PBR FBX exporters only emit DiffuseColor.
    if (!hasAlbedoSlot && !pass->getTextureUnitState("albedo")) {
        for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
            auto* tus = pass->getTextureUnitState(i);
            if (tus->getName() == "diffuse_map" && !tus->getTextureName().empty()) {
                auto* alb = pass->createTextureUnitState(tus->getTextureName());
                alb->setName("albedo");
                if (Ogre::RTShader::ShaderGenerator::getSingletonPtr()) {
                    Ogre::RTShader::ShaderGenerator::_markNonFFP(alb);
                }
                break;
            }
        }
    }

    // PBR-FBX round-trip fix: when the source FBX exposes BASE_COLOR but
    // no legacy DIFFUSE (which is what our slice F4 export does, and what
    // most PBR-authored FBX files do), the `albedo` slot becomes the
    // first FFP-eligible TUS. FFP texturing modulates that against
    // `pass->diffuse`, but PBR exporters routinely write
    // `DiffuseColor = (0,0,0)` because in PBR the base colour comes from
    // the texture, not the material colour. The result was a near-black
    // surface on reimport even though the texture itself was correct.
    //
    // If we have an albedo slot, no diffuse_map slot, and the imported
    // diffuse colour is essentially black, force it to white so the FFP
    // modulate passes the texture through unchanged. A user-set tint
    // (anything above the epsilon) is preserved.
    if (hasAlbedoSlot && !pass->getTextureUnitState("diffuse_map")) {
        const auto d = pass->getDiffuse();
        if (d.r < 0.001f && d.g < 0.001f && d.b < 0.001f) {
            pass->setDiffuse(1.0f, 1.0f, 1.0f, d.a);
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

    // Wire the slice E PBR slot colour-ops so the freshly-imported
    // material renders the same as after a no-op Apply in the Material
    // Editor (which calls wirePbrSlotsForFFP). Guarded behind the
    // render-system check because the lightweight test fixture sets up
    // Ogre::Root without a render system; setColourOperationEx +
    // subsequent compile both walk paths that segfault on that fixture.
    // In real app use the render system is always up by import time.
    if (Ogre::Root::getSingletonPtr() && Ogre::Root::getSingletonPtr()->getRenderSystem()) {
        RTShaderHelper::finalizeShaderGenMaterial(ogreMaterial, stagedNormalTex);
    }

    return ogreMaterial;
}

Ogre::TexturePtr MaterialProcessor::loadTexture(const Ogre::String &filename, const aiString &path, const aiScene* scene) const
{
    if(auto texture = scene->GetEmbeddedTexture(path.C_Str())) {
        //returned pointer is not null, read texture from memory
        if(texture->mHeight == 0) {
            // The texture data is compressed (e.g., JPEG, PNG, etc.)
            // Stash the raw compressed bytes so the FBX re-exporter can
            // embed them in Video.Content. Without this they only live
            // in the GPU texture cache and the exporter has no path
            // back to them. Issue #508. `aiTexel*` (4-byte BGRA struct
            // for raw, byte stream for compressed) → `const std::byte*`
            // is the canonical idiom for "opaque bytes".
            EmbeddedTextureCache::store(
                filename,
                reinterpret_cast<const std::byte*>(texture->pcData),
                texture->mWidth);
            Ogre::DataStreamPtr stream(new Ogre::MemoryDataStream(texture->pcData, texture->mWidth));
            Ogre::Image img;
            img.load(stream, texture->achFormatHint);

            return Ogre::TextureManager::getSingleton().loadImage(
                    filename,
                    Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
                    img
                    );
        } else {
            // The texture data is raw. Stash the raw payload too — note
            // it's *uncompressed* RGB8, which most FBX readers won't
            // accept in Video.Content. We still stash it for symmetry
            // and let the exporter re-encode if needed. Issue #508.
            const std::size_t rawBytes =
                static_cast<std::size_t>(texture->mWidth) *
                static_cast<std::size_t>(texture->mHeight) * 3;
            EmbeddedTextureCache::store(
                filename,
                reinterpret_cast<const std::byte*>(texture->pcData),
                rawBytes);
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
    // Legacy entry point — full RTSS wiring happens in finalizeShaderGenMaterial
    // after all PBR slots exist. Keep UOB for export round-trip (#508).
    if (mat && mat->getNumTechniques() > 0 && mat->getTechnique(0)->getNumPasses() > 0) {
        Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
        pass->getUserObjectBindings().setUserAny(
            "qtme.normal_map", Ogre::Any(normalMapName));
    }
    RTShaderHelper::finalizeShaderGenMaterial(mat, normalMapName);
}
