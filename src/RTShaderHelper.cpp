// LCOV_EXCL_START — requires initialized Ogre RTSS with GPU/render system

#include "RTShaderHelper.h"
#include "EmbeddedTextureCache.h"
#include "HDR/HDREnvironmentManager.h"
#include "HDR/HdrIblRtss.h"
#include "SentryReporter.h"
#include <OgreRTShaderSystem.h>
#include <OgreMaterialSerializer.h>
#include <cstring>
#include <QCoreApplication>
#include <QDir>
#include <QString>
#include <QSet>
#include <QString>

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
    if (auto* existing = Ogre::RTShader::ShaderGenerator::getSingletonPtr()) {
        if (sceneMgr)
            existing->addSceneManager(sceneMgr);
        return;
    }

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

    HdrIblRtss::registerFactory();
}

void RTShaderHelper::shutdown(Ogre::SceneManager* sceneMgr)
{
    HdrIblRtss::unregisterFactory();

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

bool tryApplyHdrIbl(Ogre::RTShader::ShaderGenerator* shaderGen,
                    Ogre::RTShader::RenderState* renderState,
                    const Ogre::Pass* srcPass,
                    const Ogre::MaterialPtr& mat)
{
    auto* hdrMgr = HDREnvironmentManager::getSingletonPtr();
    if (!hdrMgr || !hdrMgr->isIblReady())
        return false;

    const Ogre::TexturePtr irradiance = hdrMgr->irradianceMap();
    const Ogre::TexturePtr prefilter = hdrMgr->prefilteredSpecularMap();
    const Ogre::TexturePtr brdfLut = hdrMgr->brdfLut();
    if (!irradiance || !prefilter || !brdfLut)
        return false;

    auto* iblSrs = shaderGen->createSubRenderState(HdrIblRtss::SRS_QTME_HDR_IBL);
    if (!iblSrs)
        return false;

    const float intensity = HdrIblRtss::readEnvIntensity(srcPass);
    const Ogre::ColourValue tint = HdrIblRtss::readEnvTint(srcPass);
    iblSrs->setParameter("irradiance_texture", irradiance->getName());
    iblSrs->setParameter("prefilter_texture", prefilter->getName());
    iblSrs->setParameter("brdf_lut_texture", brdfLut->getName());
    iblSrs->setParameter("prefilter_max_lod",
                         Ogre::StringConverter::toString(hdrMgr->prefilterMaxLodLevel()));
    iblSrs->setParameter("env_intensity", Ogre::StringConverter::toString(intensity));
    iblSrs->setParameter("env_tint",
                         Ogre::StringUtil::format("%f %f %f", tint.r, tint.g, tint.b));

    renderState->addTemplateSubRenderState(iblSrs);

    static QSet<QString> s_boundMaterials;
    const QString matKey = QString::fromStdString(mat->getName());
    if (!s_boundMaterials.contains(matKey)) {
        s_boundMaterials.insert(matKey);
        SentryReporter::addBreadcrumb(QStringLiteral("render.hdr.bind"),
                                      QStringLiteral("material=%1").arg(matKey));
    }
    return true;
}

bool isAlbedoSlotName(const Ogre::String& name)
{
    return name == "diffuse_map" || name == "albedo" || name == "Diffuse" || name == "BaseColor";
}

bool isNormalSlotName(const Ogre::String& name)
{
    return name == "normal_map" || name == "NormalMap" || name == "Bump" || name == "bump"
        || name == "BumpMap" || name == "height_map";
}

// True for slot names that carry a non-diffuse PBR channel and therefore must
// NOT be treated as the base-colour fallback below.
static bool isKnownNonDiffuseSlot(const std::string& n)
{
    return isNormalSlotName(n) || n == "ao" || n == "emissive"
        || n == "metallic" || n == "roughness";
}

bool textureNameLooksLikeNormalMap(const Ogre::String& texName)
{
    if (texName.empty())
        return false;
    const QString q = QString::fromStdString(texName).toLower();
    return q.contains(QStringLiteral("normal")) || q.contains(QStringLiteral("bump"))
        || q.contains(QStringLiteral("nrm"));
}

void markNormalUnitNonFfp(Ogre::TextureUnitState* tus)
{
    if (!tus)
        return;
    if (auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr())
        Ogre::RTShader::ShaderGenerator::_markNonFFP(tus);
    // Prevent FFP multitexture from modulating diffuse with the normal RGB.
    tus->setColourOperationEx(Ogre::LBX_SOURCE1, Ogre::LBS_CURRENT, Ogre::LBS_CURRENT);
}

Ogre::String resolveNormalMapTextureName(Ogre::Pass* pass)
{
    if (!pass)
        return {};
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        if (isNormalSlotName(tus->getName()) && !tus->getTextureName().empty())
            return tus->getTextureName();
    }
    const auto& bindings = pass->getUserObjectBindings();
    auto any = bindings.getUserAny("qtme.normal_map");
    if (any.has_value()) {
        try {
            return Ogre::any_cast<Ogre::String>(any);
        } catch (...) {
        }
    }
    return {};
}

void dedupeAlbedoDiffuseFfp(Ogre::Pass* pass)
{
    if (!pass)
        return;
    Ogre::TextureUnitState* diffuseTus = nullptr;
    Ogre::TextureUnitState* albedoTus = nullptr;
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        if (tus->getName() == "diffuse_map")
            diffuseTus = tus;
        else if (tus->getName() == "albedo")
            albedoTus = tus;
    }
    if (!diffuseTus || !albedoTus)
        return;
    if (diffuseTus->getTextureName().empty() || albedoTus->getTextureName().empty())
        return;
    if (diffuseTus->getTextureName() != albedoTus->getTextureName())
        return;
    markNormalUnitNonFfp(diffuseTus);
}

void removeShaderGenTechnique(Ogre::MaterialPtr& mat)
{
    auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    if (!shaderGen || !mat)
        return;
    for (auto* tech : mat->getTechniques()) {
        if (tech->getSchemeName() == Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME) {
            shaderGen->removeShaderBasedTechnique(
                tech, Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME);
            break;
        }
    }
}

} // namespace

void RTShaderHelper::excludeNormalMapFromFfpChain(Ogre::MaterialPtr& mat)
{
    auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    if (!shaderGen || !mat)
        return;

    if (!mat->isLoaded()) {
        try {
            mat->load();
        } catch (...) {
            return;
        }
    }
    if (mat->getNumTechniques() == 0)
        return;
    auto* pass = mat->getTechnique(0)->getPass(0);
    if (!pass)
        return;

    Ogre::String canonicalTex;
    int16_t canonicalIdx = -1;
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        if (!isNormalSlotName(tus->getName()) || tus->getTextureName().empty())
            continue;
        canonicalIdx = static_cast<int16_t>(i);
        canonicalTex = tus->getTextureName();
        break;
    }
    if (canonicalTex.empty()) {
        const auto& bindings = pass->getUserObjectBindings();
        auto any = bindings.getUserAny("qtme.normal_map");
        if (any.has_value()) {
            try {
                canonicalTex = Ogre::any_cast<Ogre::String>(any);
            } catch (...) {
            }
        }
    }
    if (canonicalIdx < 0 && !canonicalTex.empty()) {
        for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
            auto* tus = pass->getTextureUnitState(i);
            if (isAlbedoSlotName(tus->getName()))
                continue;
            if (tus->getTextureName() == canonicalTex) {
                canonicalIdx = static_cast<int16_t>(i);
                break;
            }
        }
    }
    if (canonicalTex.empty())
        return;

    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        const auto& slot = tus->getName();
        const bool normalRole = isNormalSlotName(slot)
            || tus->getTextureName() == canonicalTex
            || textureNameLooksLikeNormalMap(tus->getTextureName());
        if (normalRole && !isAlbedoSlotName(slot))
            markNormalUnitNonFfp(tus);
    }

    for (int i = static_cast<int>(pass->getNumTextureUnitStates()) - 1; i >= 0; --i) {
        if (i == canonicalIdx)
            continue;
        auto* tus = pass->getTextureUnitState(static_cast<unsigned short>(i));
        if (isAlbedoSlotName(tus->getName()))
            continue;
        const bool extraNormalSlot = isNormalSlotName(tus->getName())
            || tus->getTextureName() == canonicalTex
            || textureNameLooksLikeNormalMap(tus->getTextureName());
        if (!extraNormalSlot)
            continue;
        pass->removeTextureUnitState(static_cast<unsigned short>(i));
        if (i < canonicalIdx)
            --canonicalIdx;
    }

    if (canonicalIdx < 0)
        return;

    auto* canonicalTus = pass->getTextureUnitState(static_cast<unsigned short>(canonicalIdx));
    canonicalTus->setName("normal_map");
    markNormalUnitNonFfp(canonicalTus);

    const Ogre::String scheme = Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME;
    auto* renderState = shaderGen->getRenderState(scheme, *mat, 0);
    if (renderState) {
        if (auto* normalSrs = renderState->getSubRenderState(Ogre::RTShader::SRS_NORMALMAP))
            normalSrs->setParameter("texture_index", std::to_string(canonicalIdx));
        shaderGen->invalidateMaterial(scheme, mat->getName());
        shaderGen->validateMaterial(scheme, *mat);
    }
}

void RTShaderHelper::finalizeShaderGenMaterial(Ogre::MaterialPtr& mat,
                                               const Ogre::String& normalMapTexName)
{
    auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    if (!shaderGen || !mat)
        return;

    if (!mat->isLoaded()) {
        try {
            mat->load();
        } catch (...) {
            return;
        }
    }
    if (mat->getNumTechniques() == 0)
        return;
    auto* pass = mat->getTechnique(0)->getPass(0);
    if (!pass)
        return;

    Ogre::String normalTex = normalMapTexName;
    if (normalTex.empty())
        normalTex = resolveNormalMapTextureName(pass);
    if (!normalTex.empty()) {
        pass->getUserObjectBindings().setUserAny("qtme.normal_map", Ogre::Any(normalTex));
    }

    excludeNormalMapFromFfpChain(mat);
    dedupeAlbedoDiffuseFfp(pass);
    removeShaderGenTechnique(mat);

    if (!normalTex.empty()) {
        applyNormalMap(mat, normalTex);
    } else {
        bool created = shaderGen->createShaderBasedTechnique(
            *mat, Ogre::MaterialManager::DEFAULT_SCHEME_NAME,
            Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME);
        if (created) {
            shaderGen->validateMaterial(Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME,
                                        *mat);
        }
    }

    wirePbrSlotsForFFP(mat.get());
    mat->compile();
}

void RTShaderHelper::bindTextureUnitsByPointer(Ogre::MaterialPtr& mat)
{
    if (!mat || mat->getNumTechniques() == 0)
        return;

    auto& tm = Ogre::TextureManager::getSingleton();
    const Ogre::String defaultGroup =
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME;

    for (auto* tech : mat->getTechniques()) {
        for (unsigned short pi = 0; pi < tech->getNumPasses(); ++pi) {
            auto* pass = tech->getPass(pi);
            if (!pass)
                continue;

            {
                const auto amb = pass->getAmbient();
                if (amb.r < 0.001f && amb.g < 0.001f && amb.b < 0.001f)
                    pass->setAmbient(Ogre::ColourValue::White);
            }

            bool hasDiffuseTexture = false;
            for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
                auto* tus = pass->getTextureUnitState(i);
                const Ogre::String texName = tus->getTextureName();
                if (texName.empty())
                    continue;

                Ogre::TexturePtr tex = tm.getByName(texName);
                if (!tex) {
                    try {
                        tex = tm.load(texName, mat->getGroup());
                    } catch (...) {
                        try {
                            tex = tm.load(texName, defaultGroup);
                        } catch (...) {
                        }
                    }
                }
                if (tex)
                    tus->setTexture(tex);

                const auto& slot = tus->getName();
                if (isAlbedoSlotName(slot)
                    || (!isKnownNonDiffuseSlot(slot) && !isNormalSlotName(slot))) {
                    hasDiffuseTexture = true;
                }
            }

            if (hasDiffuseTexture) {
                const auto d = pass->getDiffuse();
                if (d.r < 0.001f && d.g < 0.001f && d.b < 0.001f)
                    pass->setDiffuse(1.0f, 1.0f, 1.0f, d.a);
            }
        }
    }
}

static std::string sniffImageFormat(const uint8_t* data, std::size_t size)
{
    if (size >= 8 && std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0)
        return "png";
    if (size >= 2 && data[0] == 0xff && data[1] == 0xd8)
        return "jpg";
    return "png";
}

static void hydrateEmbeddedTexturesForMaterial(Ogre::MaterialPtr& mat)
{
    if (!mat || mat->getNumTechniques() == 0)
        return;

    auto& tm = Ogre::TextureManager::getSingleton();
    auto& rgm = Ogre::ResourceGroupManager::getSingleton();
    const Ogre::String matGroup = mat->getGroup();
    if (!rgm.resourceGroupExists(matGroup))
        rgm.createResourceGroup(matGroup);

    for (auto* tech : mat->getTechniques()) {
        for (unsigned short pi = 0; pi < tech->getNumPasses(); ++pi) {
            Ogre::Pass* pass = tech->getPass(pi);
            if (!pass)
                continue;
            for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
                Ogre::TextureUnitState* tus = pass->getTextureUnitState(i);
                const Ogre::String texName = tus->getTextureName();
                if (texName.empty())
                    continue;
                if (tm.getByName(texName, matGroup))
                    continue;

                const std::vector<uint8_t> bytes =
                    EmbeddedTextureCache::retrieve(texName);
                if (bytes.empty())
                    continue;

                try {
                    Ogre::DataStreamPtr stream(new Ogre::MemoryDataStream(
                        const_cast<uint8_t*>(bytes.data()),
                        bytes.size(),
                        false,
                        true));
                    Ogre::Image img;
                    img.load(stream, sniffImageFormat(bytes.data(), bytes.size()));
                    if (tm.resourceExists(texName, matGroup)) {
                        try {
                            tm.remove(texName, matGroup);
                        } catch (...) {
                        }
                    }
                    tm.loadImage(texName, matGroup, img);
                } catch (...) {
                }
            }
        }
    }
}

void RTShaderHelper::syncMaterialForViewport(Ogre::MaterialPtr& mat)
{
    if (!mat)
        return;
    if (!Ogre::Root::getSingletonPtr() || !Ogre::Root::getSingletonPtr()->getRenderSystem())
        return;

    const Ogre::String matName = mat->getName();
    const Ogre::String matGroup = mat->getGroup();

    try {
        Ogre::MaterialSerializer serializer;
        serializer.queueForExport(mat, false, false, matName);
        const std::string script = serializer.getQueuedAsString();
        if (!script.empty()) {
            if (Ogre::MaterialManager::getSingleton().resourceExists(matName, matGroup))
                Ogre::MaterialManager::getSingleton().remove(matName, matGroup);

            std::vector<char> scriptCopy(script.begin(), script.end());
            Ogre::DataStreamPtr stream(new Ogre::MemoryDataStream(
                scriptCopy.data(),
                scriptCopy.size(),
                false,
                true));
            Ogre::MaterialManager::getSingleton().parseScript(stream, matGroup);
            mat = Ogre::MaterialManager::getSingleton().getByName(matName, matGroup);
        }
    } catch (...) {
    }

    if (!mat)
        return;

    if (!mat->isLoaded()) {
        try {
            mat->load();
        } catch (...) {
            return;
        }
    }
    if (mat->getNumTechniques() == 0)
        return;

    hydrateEmbeddedTexturesForMaterial(mat);
    bindTextureUnitsByPointer(mat);

    auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    if (shaderGen) {
        shaderGen->removeAllShaderBasedTechniques(
            matName, Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
    }

    wirePbrSlotsForFFP(mat.get());
    mat->compile();
    applyPbrIfTagged(mat);

    if (mat->getNumTechniques() > 0) {
        auto* pass = mat->getTechnique(0)->getPass(0);
        if (pass) {
            for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
                auto* tus = pass->getTextureUnitState(i);
                const auto& tusName = tus->getName();
                if (tusName == "normal_map" || tusName == "NormalMap") {
                    const std::string texName = tus->getTextureName();
                    if (!texName.empty())
                        applyNormalMap(mat, texName);
                    break;
                }
            }
        }
    }

    if (shaderGen) {
        shaderGen->validateMaterial(Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME, *mat);
    }
}

void RTShaderHelper::refreshMaterialForViewport(Ogre::MaterialPtr& mat)
{
    if (!mat)
        return;
    if (!Ogre::Root::getSingletonPtr() || !Ogre::Root::getSingletonPtr()->getRenderSystem())
        return;
    if (!mat->isLoaded()) {
        try {
            mat->load();
        } catch (...) {
            return;
        }
    }
    if (mat->getNumTechniques() == 0)
        return;

    auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    if (shaderGen) {
        shaderGen->removeAllShaderBasedTechniques(
            mat->getName(), Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
    }

    wirePbrSlotsForFFP(mat.get());
    mat->compile();
    applyPbrIfTagged(mat);

    auto* pass = mat->getTechnique(0)->getPass(0);
    if (pass) {
        for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
            auto* tus = pass->getTextureUnitState(i);
            const auto& tusName = tus->getName();
            if (tusName == "normal_map" || tusName == "NormalMap") {
                const std::string texName = tus->getTextureName();
                if (!texName.empty())
                    applyNormalMap(mat, texName);
                break;
            }
        }
    }

    if (shaderGen) {
        shaderGen->validateMaterial(Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME, *mat);
    }
}

void RTShaderHelper::wirePbrSlotsForFFP(Ogre::Material* mat)
{
    if (!mat) return;
    for (auto* tech : mat->getTechniques()) {
        for (unsigned short pi = 0; pi < tech->getNumPasses(); ++pi) {
            auto* p = tech->getPass(pi);

            // Does this pass expose a recognized diffuse/base-colour slot? If
            // not, the first plain textured slot (numeric/empty name, e.g. an
            // Assimp-imported "0" TUS or a user-added unit) must still receive
            // the diffuse modulate op — otherwise a texture applied to it loads
            // but never renders, since the loop below only wires NAMED slots.
            // (This was the "applying texture to the model does nothing" bug.)
            bool hasNamedDiffuse = false;
            int fallbackDiffuse = -1;
            for (unsigned short i = 0; i < p->getNumTextureUnitStates(); ++i) {
                auto* tus = p->getTextureUnitState(i);
                const std::string& n = tus->getName();
                if (isAlbedoSlotName(n)) {
                    hasNamedDiffuse = true;
                    break;
                }
                if (fallbackDiffuse < 0 && !isKnownNonDiffuseSlot(n)
                    && !tus->getTextureName().empty()) {
                    fallbackDiffuse = i;
                }
            }

            for (unsigned short i = 0; i < p->getNumTextureUnitStates(); ++i) {
                auto* tus = p->getTextureUnitState(i);
                const std::string& n = tus->getName();
                if (isNormalSlotName(n)) {
                    markNormalUnitNonFfp(tus);
                } else if (!hasNamedDiffuse && i == fallbackDiffuse) {
                    // Unnamed/numeric diffuse fallback — wire like albedo.
                    tus->setColourOperationEx(
                        Ogre::LBX_MODULATE,
                        Ogre::LBS_TEXTURE,
                        Ogre::LBS_DIFFUSE);
                } else if (isAlbedoSlotName(n)) {
                    tus->setColourOperationEx(
                        Ogre::LBX_MODULATE,
                        Ogre::LBS_TEXTURE,
                        Ogre::LBS_DIFFUSE);
                } else if (n == "ao") {
                    tus->setColourOperationEx(
                        Ogre::LBX_MODULATE,
                        Ogre::LBS_TEXTURE,
                        Ogre::LBS_DIFFUSE);
                } else if (n == "emissive") {
                    tus->setColourOperationEx(
                        Ogre::LBX_ADD,
                        Ogre::LBS_TEXTURE,
                        Ogre::LBS_CURRENT);
                } else if (n == "metallic") {
                    // FFP approximation: signed-add brightens current
                    // toward white in textured (metal) regions. Slice F
                    // shader replaces with a real metal BRDF lobe when
                    // pbr_workflow is tagged.
                    tus->setColourOperationEx(
                        Ogre::LBX_ADD_SIGNED,
                        Ogre::LBS_TEXTURE,
                        Ogre::LBS_CURRENT);
                } else if (n == "roughness") {
                    // FFP approximation: modulate-x2 brightens smooth
                    // (low-roughness) regions.
                    tus->setColourOperationEx(
                        Ogre::LBX_MODULATE_X2,
                        Ogre::LBS_TEXTURE,
                        Ogre::LBS_CURRENT);
                }
            }
        }
    }
}

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
    //
    // Two carve-outs covered here:
    //   1. The caller may have passed a `normalMapTexName` (e.g. Assimp
    //      import discovering a normal texture before any TUS holds it).
    //      Seed the empty `normal_map` slot with that texture so
    //      applyPbrIfTagged picks it up via textureOnSlot.
    //   2. applyPbrIfTagged can return false (SRS unavailable, render
    //      state missing, etc.). In that case we MUST fall through to
    //      the legacy SRS_NORMALMAP path, otherwise PBR-tagged materials
    //      silently lose all normal-map detail when Cook-Torrance can't
    //      be attached.
    if (isMetallicRoughnessMaterial(mat) && mat->getNumTechniques() > 0) {
        if (!normalMapTexName.empty()) {
            auto* pbrPass = mat->getTechnique(0)->getPass(0);
            for (unsigned short i = 0; i < pbrPass->getNumTextureUnitStates(); ++i) {
                auto* tus = pbrPass->getTextureUnitState(i);
                const auto& n = tus->getName();
                if ((n == "normal_map" || n == "NormalMap") && tus->getTextureName().empty()) {
                    tus->setTextureName(normalMapTexName);
                    break;
                }
            }
        }
        if (applyPbrIfTagged(mat)) {
            return;
        }
        // applyPbrIfTagged failed (e.g., Cook-Torrance SRS unavailable).
        // Fall through to the legacy path so the user at least gets
        // normal-map shading.
        Ogre::LogManager::getSingleton().logMessage(
            "RTShaderHelper: PBR composition failed for '" + mat->getName() +
            "' — falling back to plain SRS_NORMALMAP wiring");
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

        excludeNormalMapFromFfpChain(mat);

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

        tryApplyHdrIbl(shaderGen, renderState, srcPass, mat);

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

void RTShaderHelper::refreshAllPbrMaterialsForHdr()
{
    if (!Ogre::RTShader::ShaderGenerator::getSingletonPtr())
        return;

    auto& materialMgr = Ogre::MaterialManager::getSingleton();
    auto it = materialMgr.getResourceIterator();
    while (it.hasMoreElements()) {
        Ogre::MaterialPtr mat =
            Ogre::static_pointer_cast<Ogre::Material>(it.getNext());
        if (!mat || !isMetallicRoughnessMaterial(mat))
            continue;
        applyPbrIfTagged(mat);
    }
}

void RTShaderHelper::setPbrEnvIntensity(Ogre::Pass* pass, float intensity)
{
    if (!pass)
        return;
    pass->getUserObjectBindings().setUserAny(HdrIblRtss::kPbrEnvIntensityKey, Ogre::Any(intensity));
}

void RTShaderHelper::setPbrEnvTint(Ogre::Pass* pass, const Ogre::ColourValue& tint)
{
    if (!pass)
        return;
    pass->getUserObjectBindings().setUserAny(HdrIblRtss::kPbrEnvTintKey, Ogre::Any(tint));
}

float RTShaderHelper::pbrEnvIntensity(const Ogre::Pass* pass)
{
    return HdrIblRtss::readEnvIntensity(pass);
}

Ogre::ColourValue RTShaderHelper::pbrEnvTint(const Ogre::Pass* pass)
{
    return HdrIblRtss::readEnvTint(pass);
}
