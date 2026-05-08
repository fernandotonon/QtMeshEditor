// LCOV_EXCL_START — requires initialized Ogre RTSS with GPU/render system

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

    // Collect candidate directories. The macOS dev build keeps two copies
    // of media/ (one inside the .app bundle, one alongside it for cmake
    // --install) — they have IDENTICAL contents, so registering both
    // re-parses every .program script and trips ItemIdentityException on
    // duplicate GpuProgram declarations (e.g. Ogre/ShadowBlendVP). Pick
    // the FIRST candidate that exists and stop.
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

    // Pick the first existing RTShaderLib + sibling Main pair.
    QString chosenLib;
    QString chosenMain;
    for (const auto& path : candidates) {
        const QString canonLib = QDir(path).canonicalPath();
        if (canonLib.isEmpty()) continue;
        const QString sibMain = QDir(path + "/..").absolutePath() + "/Main";
        const QString canonMain = QDir(sibMain).canonicalPath();
        if (canonMain.isEmpty()) continue;
        chosenLib = canonLib;
        chosenMain = canonMain;
        break;
    }

    if (chosenLib.isEmpty()) {
        log.logMessage("RTSS: No RTShaderLib directory found — shaders will fail to load");
        return;
    }

    log.logMessage("RTSS: Adding resource location: " + chosenLib.toStdString());
    rgm.addResourceLocation(chosenLib.toStdString(), "FileSystem", Ogre::RGN_INTERNAL);
    log.logMessage("RTSS: Adding resource location: " + chosenMain.toStdString());
    rgm.addResourceLocation(chosenMain.toStdString(), "FileSystem", Ogre::RGN_INTERNAL);
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

namespace {

/// Look up the slice E `pbr_workflow` user-binding key on a pass and
/// return the workflow string ("metallic_roughness", "specular_glossiness",
/// "unlit") or empty if absent. Mirrors the key constant defined in
/// MaterialPresetLibrary.h without taking a header dependency on it.
constexpr const char* kPbrWorkflowKey = "pbr_workflow";

Ogre::String readPbrWorkflowTag(const Ogre::Pass* pass)
{
    if (!pass) return {};
    const auto& bindings = pass->getUserObjectBindings();
    auto any = bindings.getUserAny(kPbrWorkflowKey);
    if (!any.has_value()) return {};
    try { return Ogre::any_cast<Ogre::String>(any); }
    catch (...) { return {}; }
}

/// Find the texture name on a TUS slot by name. Returns empty if the
/// slot doesn't exist or has no texture set.
Ogre::String textureOnSlot(const Ogre::Pass* pass, const Ogre::String& slotName)
{
    if (!pass) return {};
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        const auto* tus = pass->getTextureUnitState(i);
        if (tus->getName() == slotName)
            return tus->getTextureName();
    }
    return {};
}

bool hasNamedTUS(const Ogre::Pass* pass, const Ogre::String& slotName)
{
    if (!pass) return false;
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        if (pass->getTextureUnitState(i)->getName() == slotName)
            return true;
    }
    return false;
}

/// MaterialSerializer does NOT serialize Pass::UserObjectBindings, so the
/// pbr_workflow tag is lost on script round-trip (e.g., user edits the
/// material text and clicks Apply). Fall back to detecting PBR layout by
/// the canonical slot names: a pass with all 6 named slots is treated as
/// metallic-roughness PBR.
bool detectMetallicRoughnessByLayout(const Ogre::Pass* pass)
{
    return hasNamedTUS(pass, "albedo")
        && hasNamedTUS(pass, "normal_map")
        && hasNamedTUS(pass, "metallic")
        && hasNamedTUS(pass, "roughness")
        && hasNamedTUS(pass, "ao")
        && hasNamedTUS(pass, "emissive");
}

bool isMetallicRoughnessMaterial(const Ogre::MaterialPtr& mat)
{
    if (!mat || mat->getNumTechniques() == 0) return false;
    auto* pass = mat->getTechnique(0)->getPass(0);
    const Ogre::String tag = readPbrWorkflowTag(pass);
    return tag == "metallic_roughness"
        || (tag.empty() && detectMetallicRoughnessByLayout(pass));
}

} // namespace

void RTShaderHelper::applyNormalMap(Ogre::MaterialPtr& mat, const std::string& normalMapTexName)
{
    auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    if (!shaderGen) return;

    // Slice F2: when the material is PBR-tagged (or matches the PBR
    // layout), defer to applyPbrIfTagged so the normal map is composed
    // with SRS_COOK_TORRANCE_LIGHTING in a single render-state cycle.
    // Without this redirect, applyNormalMap's resetToBuiltinSubRenderStates
    // would replace the Cook-Torrance SRS, dropping PBR shading whenever
    // a normal map gets re-applied (e.g. on import or material text edit).
    if (isMetallicRoughnessMaterial(mat)) {
        applyPbrIfTagged(mat);
        return;
    }

    try {
        // Verify the normal map texture is registered (exists in any resource group).
        // We do NOT require isLoaded() here: textures loaded from a .material file
        // alongside a .mesh are in a path-based group and getByName() may return
        // an unloaded stub even though the texture data is already in GPU memory.
        // RTSS will bind to the TUS which already holds the texture pointer.
        {
            auto normalTex = Ogre::TextureManager::getSingleton().getByName(normalMapTexName);
            if (!normalTex) {
                // Texture not registered at all — try loading it now.
                try {
                    Ogre::TextureManager::getSingleton().load(
                        normalMapTexName,
                        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
                } catch (...) {}
                normalTex = Ogre::TextureManager::getSingleton().getByName(normalMapTexName);
            }
            if (!normalTex) {
                Ogre::LogManager::getSingleton().logMessage(
                    "RTShaderHelper: Skipping normal map — texture '" + normalMapTexName + "' not found");
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

        // Do not add SRS_PER_PIXEL_LIGHTING here: the ShaderGenerator scheme's global
        // RenderState already includes it (resetToBuiltinSubRenderStates). Adding another
        // replaces that instance during TargetRenderState::link() with a fresh default,
        // which breaks lighting + normal map generation.

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

// LCOV_EXCL_STOP

bool RTShaderHelper::applyPbrIfTagged(Ogre::MaterialPtr& mat)
{
    if (!mat || mat->getNumTechniques() == 0) return false;
    auto* srcPass = mat->getTechnique(0)->getPass(0);
    Ogre::String workflow = readPbrWorkflowTag(srcPass);

    // MaterialSerializer drops UserObjectBindings, so the tag is missing
    // after parseScript round-trips. Fall back to slot-layout detection
    // when the tag is absent. Specular-Glossiness and Unlit aren't yet
    // distinguishable from layout alone, so layout-detection only
    // promotes to metallic_roughness.
    if (workflow.empty() && detectMetallicRoughnessByLayout(srcPass)) {
        workflow = "metallic_roughness";
    }

    // Slice F1 wires only metallic_roughness via Ogre's stock SRS.
    // Specular-Glossiness and Unlit keep slice E's FFP wiring.
    if (workflow != "metallic_roughness") return false;

    auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    if (!shaderGen) return false;

    // Helper to drop any RTSS technique we may have created so the
    // material returns to a clean FFP-only state on bail-out paths.
    // Without this, a partial technique created by
    // createShaderBasedTechnique persists when getRenderState or
    // createSubRenderState fail later — handleSchemeNotFound then
    // validates it as plain Phong, silently overriding the FFP fallback.
    auto cleanupRtssTech = [&]() {
        if (!mat) return;
        for (auto* t : mat->getTechniques()) {
            if (t->getSchemeName() ==
                Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME) {
                shaderGen->removeShaderBasedTechnique(
                    t, Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME);
                break;
            }
        }
    };

    try {
        if (!mat->isLoaded())
            mat->load();

        // Drop any pre-existing RTSS technique so we replace it with
        // the Cook-Torrance one cleanly. Without this, an FFP-derived
        // RTSS technique from handleSchemeNotFound persists alongside.
        Ogre::Technique* srcTech = nullptr;
        for (auto* t : mat->getTechniques()) {
            if (t->getSchemeName() == Ogre::MaterialManager::DEFAULT_SCHEME_NAME) {
                srcTech = t;
                break;
            }
        }
        if (srcTech) {
            shaderGen->removeShaderBasedTechnique(
                srcTech, Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME);
        }

        bool created = shaderGen->createShaderBasedTechnique(
            *mat,
            Ogre::MaterialManager::DEFAULT_SCHEME_NAME,
            Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME);
        if (!created) {
            Ogre::LogManager::getSingleton().logMessage(
                "RTShaderHelper: PBR technique creation failed for '" + mat->getName() + "'");
            return false;
        }

        auto* renderState = shaderGen->getRenderState(
            Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME, *mat, 0);
        if (!renderState) {
            cleanupRtssTech();
            return false;
        }

        // Reset to built-in SRSes so we don't accumulate stale state
        // (e.g., a previous SRS_NORMALMAP with a different texture_index).
        renderState->resetToBuiltinSubRenderStates();

        auto* pbrSRS = shaderGen->createSubRenderState(
            Ogre::RTShader::SRS_COOK_TORRANCE_LIGHTING);
        if (!pbrSRS) {
            Ogre::LogManager::getSingleton().logMessage(
                "RTShaderHelper: SRS_COOK_TORRANCE_LIGHTING unavailable in this Ogre build");
            cleanupRtssTech();
            return false;
        }

        // glTF MR-texture convention: a single RGBA texture with
        // .r=AO, .g=roughness, .b=metallic. If the user populated the
        // `metallic` slot (most common), use that as the packed map.
        // The SRS's `texture` parameter expects the texture *name*.
        const Ogre::String mrTexName = textureOnSlot(srcPass, "metallic");
        if (!mrTexName.empty()) {
            pbrSRS->setParameter("texture", mrTexName);
        }
        // No mr texture → SRS uses ACT_SURFACE_SPECULAR_COLOUR.xy as
        // metal/roughness scalars. We could route the pass shininess
        // here in a future revision, but the Phong approximation set
        // by MaterialPresetLibrary already feeds setSpecular/setShininess.

        renderState->addTemplateSubRenderState(pbrSRS);

        // Slice F2: chain SRS_NORMALMAP when the pass has a `normal_map`
        // TUS with a texture set, so PBR materials with normal maps get
        // both effects. NormalMapLighting's getExecutionOrder is
        // FFP_LIGHTING - 1, so it runs before Cook-Torrance and supplies
        // the per-pixel normal that Cook-Torrance will then sample.
        // Without this, a PBR-tagged material with a normal map gets only
        // the Cook-Torrance shader (no per-pixel normal) — the previous
        // applyNormalMap call's render-state was reset above.
        const Ogre::String normalMapTexName = textureOnSlot(srcPass, "normal_map");
        if (!normalMapTexName.empty()) {
            // Find the slot index for the SRS's texture_index parameter.
            int16_t normalMapIdx = -1;
            for (unsigned short i = 0; i < srcPass->getNumTextureUnitStates(); ++i) {
                const auto& n = srcPass->getTextureUnitState(i)->getName();
                if (n == "normal_map" || n == "NormalMap") {
                    normalMapIdx = static_cast<int16_t>(i);
                    break;
                }
            }
            if (normalMapIdx >= 0) {
                // Mark the normal-map TUS non-FFP so the FFP texturing
                // chain doesn't sample it as a regular diffuse layer.
                Ogre::RTShader::ShaderGenerator::_markNonFFP(
                    srcPass->getTextureUnitState(normalMapIdx));

                auto* normalSRS = shaderGen->createSubRenderState(
                    Ogre::RTShader::SRS_NORMALMAP);
                if (normalSRS) {
                    normalSRS->setParameter("texture_index",
                                            std::to_string(normalMapIdx));
                    renderState->addTemplateSubRenderState(normalSRS);
                }
            }
        }

        shaderGen->invalidateMaterial(
            Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME, mat->getName());
        shaderGen->validateMaterial(
            Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME, *mat);

        Ogre::LogManager::getSingleton().logMessage(
            "RTShaderHelper: PBR (Cook-Torrance) applied to '" + mat->getName() +
            (mrTexName.empty() ? "' [no MR texture]" : "' tex='" + mrTexName + "'") +
            (normalMapTexName.empty() ? "" : " + normal map '" + normalMapTexName + "'"));
        return true;
    } catch (const Ogre::Exception& e) {
        Ogre::LogManager::getSingleton().logMessage(
            "RTShaderHelper: Ogre exception applying PBR: " + e.getDescription());
    } catch (const std::exception& e) {
        Ogre::LogManager::getSingleton().logMessage(
            "RTShaderHelper: Exception applying PBR: " + std::string(e.what()));
    }
    return false;
}
