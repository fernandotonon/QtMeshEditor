#include "RTShaderHelper.h"
#include <OgreRTShaderSystem.h>
#include <QCoreApplication>
#include <QDir>
#include <QSet>

// Scheme resolver listener — generates RTSS shader techniques on demand
// when Ogre can't find a technique for the ShaderGenerator scheme.
namespace {
class SchemeResolverListener : public Ogre::MaterialManager::Listener {
    Ogre::RTShader::ShaderGenerator* mShaderGen;
public:
    explicit SchemeResolverListener(Ogre::RTShader::ShaderGenerator* sg) : mShaderGen(sg) {}

    Ogre::Technique* handleSchemeNotFound(unsigned short, const Ogre::String& schemeName,
                                          Ogre::Material* mat, unsigned short,
                                          const Ogre::Renderable*) override
    {
        if (!mShaderGen->hasRenderState(schemeName))
            return nullptr;

        // Skip unlit / overlay materials (e.g. gizmos, grid, selection box)
        // — RTSS would replace their vertex-color pass-through with lighting shaders
        if (mat->getNumTechniques() > 0) {
            auto* tech = mat->getTechnique(0);
            if (tech->getNumPasses() > 0 && !tech->getPass(0)->getLightingEnabled())
                return nullptr;
        }

        // PBR-workflow models often have ambient=(0,0,0), which kills ambient
        // lighting in Ogre's Phong model.  Fix before RTSS generates shaders.
        if (mat->getNumTechniques() > 0) {
            auto* pass = mat->getTechnique(0)->getPass(0);
            auto amb = pass->getAmbient();
            if (amb.r < 0.001f && amb.g < 0.001f && amb.b < 0.001f)
                pass->setAmbient(Ogre::ColourValue::White);
        }

        // If an RTSS technique already exists (e.g. from applyNormalMap()),
        // just validate and return it — don't create a duplicate without the normal map SRS
        for (auto* t : mat->getTechniques()) {
            if (t->getSchemeName() == schemeName) {
                mShaderGen->validateMaterial(schemeName, *mat);
                return t;
            }
        }

        bool created = mShaderGen->createShaderBasedTechnique(
            *mat, Ogre::MaterialManager::DEFAULT_SCHEME_NAME, schemeName);
        if (!created)
            return nullptr;

        mShaderGen->validateMaterial(schemeName, *mat);

        for (auto* t : mat->getTechniques()) {
            if (t->getSchemeName() == schemeName)
                return t;
        }
        return nullptr;
    }

    bool afterIlluminationPassesCreated(Ogre::Technique* tech) override {
        if (mShaderGen->hasRenderState(tech->getSchemeName())) {
            auto* mat = tech->getParent();
            mShaderGen->validateMaterialIlluminationPasses(
                tech->getSchemeName(), mat->getName(), mat->getGroup());
            return true;
        }
        return false;
    }

    bool beforeIlluminationPassesCleared(Ogre::Technique* tech) override {
        if (mShaderGen->hasRenderState(tech->getSchemeName())) {
            auto* mat = tech->getParent();
            mShaderGen->invalidateMaterialIlluminationPasses(
                tech->getSchemeName(), mat->getName(), mat->getGroup());
            return true;
        }
        return false;
    }
};

static SchemeResolverListener* sListener = nullptr;

// Add RTSS shader library resource locations
static void addRTSSResources()
{
    auto& rgm = Ogre::ResourceGroupManager::getSingleton();
    auto& log = Ogre::LogManager::getSingleton();

    // Collect candidate directories, then deduplicate via canonical paths
    QStringList candidates;
    QString appDir = QCoreApplication::applicationDirPath();

    // Bundled locations (covers macOS .app bundle and Linux/Windows layouts)
    candidates << appDir + "/media/RTShaderLib"
               << appDir + "/../media/RTShaderLib";

#if OGRE_PLATFORM == OGRE_PLATFORM_APPLE
    // macOS .app bundle: appDir is Contents/MacOS/, so ../.. is the bundle root
    candidates << appDir + "/../../media/RTShaderLib";
    // macOS dev builds: cmake --install puts media at bin/media/ while the
    // binary is 3 levels deeper at bin/QtMeshEditor.app/Contents/MacOS/
    candidates << appDir + "/../../../media/RTShaderLib";
#endif

    // For each RTShaderLib candidate, also try its sibling "Main" dir
    // (contains OgreUnifiedShader.h needed by RTSS)
    QStringList withMain = candidates;
    for (const auto& c : candidates)
        withMain << QDir(c + "/..").absolutePath() + "/Main";

    // Deduplicate using canonical paths and add existing directories
    QSet<QString> added;
    for (const auto& path : withMain) {
        QString canon = QDir(path).canonicalPath();
        if (canon.isEmpty() || added.contains(canon))
            continue;
        added.insert(canon);
        log.logMessage("RTSS: Adding resource location: " + canon.toStdString());
        rgm.addResourceLocation(canon.toStdString(), "FileSystem", Ogre::RGN_INTERNAL);
    }
}
}

void RTShaderHelper::initialize(Ogre::SceneManager* sceneMgr)
{
    if (!Ogre::RTShader::ShaderGenerator::initialize())
    {
        Ogre::LogManager::getSingleton().logMessage("RTSS: ShaderGenerator failed to initialize");
        return;
    }
    auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    shaderGen->addSceneManager(sceneMgr);
    // Add RTSS shader library resources
    addRTSSResources();

    // Register scheme resolver so RTSS techniques are generated on demand
    sListener = new SchemeResolverListener(shaderGen);
    Ogre::MaterialManager::getSingleton().addListener(sListener);
}

void RTShaderHelper::shutdown(Ogre::SceneManager* sceneMgr)
{
    if (sListener) {
        Ogre::MaterialManager::getSingleton().removeListener(sListener);
        delete sListener;
        sListener = nullptr;
    }

    auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    if (shaderGen)
    {
        if (sceneMgr)
            shaderGen->removeSceneManager(sceneMgr);
        Ogre::RTShader::ShaderGenerator::destroy();
    }
}

void RTShaderHelper::applyNormalMap(Ogre::MaterialPtr& mat, const std::string& normalMapTexName)
{
    auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    if (!shaderGen) return;

    try {
        // Verify the normal map texture actually exists and is loaded.
        // If the texture file is missing, applying the RTSS normal map SRS would
        // produce garbage lighting (shader reads blank/default texture data).
        {
            auto normalTex = Ogre::TextureManager::getSingleton().getByName(normalMapTexName);
            if (!normalTex || !normalTex->isLoaded()) {
                Ogre::LogManager::getSingleton().logMessage(
                    "RTShaderHelper: Skipping normal map — texture '" + normalMapTexName + "' not loaded");
                return;
            }
        }

        // Ensure material is fully loaded (script-parsed materials may not be loaded yet,
        // which means getSupportedTechniques() is empty and RTSS can't find a source technique)
        if (!mat->isLoaded())
            mat->load();

        // Fix black ambient from PBR-workflow models
        auto* srcPass = mat->getTechnique(0)->getPass(0);
        {
            auto amb = srcPass->getAmbient();
            if (amb.r < 0.001f && amb.g < 0.001f && amb.b < 0.001f)
                srcPass->setAmbient(Ogre::ColourValue::White);
        }

        // Check if a normal map TUS already exists (handles both naming conventions)
        int16_t existingNormalIdx = -1;
        for (unsigned short i = 0; i < srcPass->getNumTextureUnitStates(); ++i) {
            const auto& tusName = srcPass->getTextureUnitState(i)->getName();
            if (tusName == "normal_map" || tusName == "NormalMap") {
                existingNormalIdx = static_cast<int16_t>(i);
                break;
            }
        }

        // Look up the texture by pointer to avoid resource-group mismatch when the
        // texture was loaded from embedded FBX data (different group than the material).
        auto normalTex = Ogre::TextureManager::getSingleton().getByName(normalMapTexName);

        // Set up the normal map TUS and mark non-FFP texture units BEFORE
        // createShaderBasedTechnique, so that RTSS sees the correct set of FFP
        // texture units from the start (avoids subtle brightness differences).
        uint16_t normalMapIdx;
        if (existingNormalIdx >= 0) {
            // Reuse existing normal map TUS (from .material script or previous import)
            normalMapIdx = static_cast<uint16_t>(existingNormalIdx);
            auto* existingTUS = srcPass->getTextureUnitState(normalMapIdx);
            if (normalTex)
                existingTUS->setTexture(normalTex);
            existingTUS->setName("normal_map");
            Ogre::RTShader::ShaderGenerator::_markNonFFP(existingTUS);
        } else {
            // Add a new normal map TUS — set texture by pointer, not by name,
            // to bypass resource group lookup that fails for embedded textures.
            auto* normalMapTUS = srcPass->createTextureUnitState();
            if (normalTex)
                normalMapTUS->setTexture(normalTex);
            else
                normalMapTUS->setTextureName(normalMapTexName);
            normalMapTUS->setName("normal_map");
            normalMapIdx = srcPass->getNumTextureUnitStates() - 1;
            Ogre::RTShader::ShaderGenerator::_markNonFFP(normalMapTUS);
        }

        // Mark any specular map TUS as non-FFP so RTSS doesn't treat it as a diffuse layer
        for (unsigned short i = 0; i < srcPass->getNumTextureUnitStates(); ++i) {
            const auto& tusName = srcPass->getTextureUnitState(i)->getName();
            if (tusName == "SpecularMap" || tusName == "specular_map")
                Ogre::RTShader::ShaderGenerator::_markNonFFP(srcPass->getTextureUnitState(i));
        }

        // Remove any existing RTSS technique so we can recreate it with the normal map SRS.
        // If handleSchemeNotFound already created a plain RTSS technique (without normal map),
        // we must remove it first — otherwise the old shader programs persist.
        {
            Ogre::Technique* srcTech = nullptr;
            for (auto* t : mat->getTechniques()) {
                if (t->getSchemeName() == Ogre::MaterialManager::DEFAULT_SCHEME_NAME) {
                    srcTech = t;
                    break;
                }
            }
            if (srcTech)
                shaderGen->removeShaderBasedTechnique(srcTech,
                    Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME);
        }

        bool created = shaderGen->createShaderBasedTechnique(
            *mat,
            Ogre::MaterialManager::DEFAULT_SCHEME_NAME,
            Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME);

        if (!created) {
            Ogre::LogManager::getSingleton().logMessage(
                "RTShaderHelper: Failed to create shader technique for '" + mat->getName() + "'");
            return;
        }

        auto* renderState = shaderGen->getRenderState(
            Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME,
            *mat, 0);

        if (!renderState) {
            Ogre::LogManager::getSingleton().logMessage(
                "RTShaderHelper: No render state for '" + mat->getName() + "'");
            return;
        }

        auto* perPixelSRS = shaderGen->createSubRenderState(
            Ogre::RTShader::SRS_PER_PIXEL_LIGHTING);
        renderState->addTemplateSubRenderState(perPixelSRS);

        auto* normalMapSRS = shaderGen->createSubRenderState(
            Ogre::RTShader::SRS_NORMALMAP);
        normalMapSRS->setParameter("texture_index", std::to_string(normalMapIdx));
        renderState->addTemplateSubRenderState(normalMapSRS);

        // Generate the actual shader programs
        shaderGen->invalidateMaterial(
            Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME,
            mat->getName());
        shaderGen->validateMaterial(
            Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME,
            *mat);

        Ogre::LogManager::getSingleton().logMessage(
            "RTShaderHelper: Normal map '" + normalMapTexName + "' applied to '" + mat->getName() + "'");
    } catch (const Ogre::Exception& e) {
        Ogre::LogManager::getSingleton().logMessage(
            "RTShaderHelper: Ogre exception applying normal map: " + e.getDescription());
    } catch (const std::exception& e) {
        Ogre::LogManager::getSingleton().logMessage(
            "RTShaderHelper: Exception applying normal map: " + std::string(e.what()));
    }
}
