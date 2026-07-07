#include "MaterialPresetLibrary.h"
#include "GamificationManager.h"
#include "HDR/HDREnvironmentManager.h"
#include "Manager.h"
#include "RTShaderHelper.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "commands/TransformCommands.h"
#include <Ogre.h>
#include <OgreRTShaderSystem.h>
#include <QHash>

MaterialPresetLibrary* MaterialPresetLibrary::m_pSingleton = nullptr;

MaterialPresetLibrary* MaterialPresetLibrary::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new MaterialPresetLibrary();
    return m_pSingleton;
}

MaterialPresetLibrary* MaterialPresetLibrary::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine);
    Q_UNUSED(scriptEngine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void MaterialPresetLibrary::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

MaterialPresetLibrary::MaterialPresetLibrary() : QObject(nullptr) {}

const char* MaterialPresetLibrary::kPbrWorkflowKey      = "pbr_workflow";
const char* MaterialPresetLibrary::kPbrWorkflowMetallic = "metallic_roughness";
const char* MaterialPresetLibrary::kPbrWorkflowSpecular = "specular_glossiness";
const char* MaterialPresetLibrary::kPbrWorkflowUnlit    = "unlit";

namespace {

// Canonical PBR texture-unit slot order. The runtime shading is
// Phong-approximated in slice E; slice F can read these slot names
// plus the pbr_workflow user binding to swap in a real PBR sub-render
// state without re-creating the material.
constexpr const char* kPbrSlots[] = {
    "albedo",      // base colour / diffuse
    "normal_map",  // tangent-space normal — name matches RTShaderHelper::applyNormalMap
    "metallic",    // metallic-roughness workflow only (specular-glossiness reuses for specular)
    "roughness",   // metallic-roughness workflow only (specular-glossiness reuses for glossiness)
    "ao",          // ambient occlusion
    "emissive"     // emissive map
};

// Configure a Pass with the six canonical PBR texture-unit slots in
// order. Slots are created with names only (no texture file) so the
// user can drag textures in via the existing Material Editor inspector.
// The albedo slot is given a 1×1 white fallback so the material renders
// before any texture is assigned.
void configurePbrSlots(Ogre::Pass* pass, const QString& workflow)
{
    // The `metallic`/`roughness` slots are canonical texture-unit names reused
    // across workflows. In METALLIC-ROUGHNESS they are BRDF specular-lobe
    // inputs (the real response comes from applyPbrIfTagged's Cook-Torrance
    // SRS) — modulating them into the FFP diffuse only darkens the surface, so
    // they're kept inert. In SPECULAR-GLOSSINESS the same two slots carry the
    // specular colour + glossiness, and that workflow stays on the FFP path
    // (applyPbrIfTagged skips it), so their FFP colour ops are the ONLY thing
    // that makes those maps visible — keep the legacy approximations there.
    const bool metalRough =
        workflow != MaterialPresetLibrary::kPbrWorkflowSpecular;
    for (const char* slotName : kPbrSlots) {
        Ogre::TextureUnitState* tus = pass->createTextureUnitState();
        tus->setName(slotName);
        // FFP approximations — see wirePbrSlotsForFFP in
        // MaterialEditorQML.cpp for the rationale per slot. Slice F
        // replaces these with a real PBR SubRenderState.
        const std::string n(slotName);
        if (n == "normal_map") {
            Ogre::RTShader::ShaderGenerator::_markNonFFP(tus);
        } else if (n == "albedo") {
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
            if (metalRough) {
                // BRDF input — inert in FFP (real response via Cook-Torrance).
                Ogre::RTShader::ShaderGenerator::_markNonFFP(tus);
                tus->setColourOperationEx(
                    Ogre::LBX_SOURCE1, Ogre::LBS_CURRENT, Ogre::LBS_CURRENT);
            } else {
                // Spec-gloss: this slot is the SPECULAR map — brighten.
                tus->setColourOperationEx(
                    Ogre::LBX_ADD_SIGNED, Ogre::LBS_TEXTURE, Ogre::LBS_CURRENT);
            }
        } else if (n == "roughness") {
            if (metalRough) {
                Ogre::RTShader::ShaderGenerator::_markNonFFP(tus);
                tus->setColourOperationEx(
                    Ogre::LBX_SOURCE1, Ogre::LBS_CURRENT, Ogre::LBS_CURRENT);
            } else {
                // Spec-gloss: this slot is the GLOSSINESS map.
                tus->setColourOperationEx(
                    Ogre::LBX_MODULATE_X2, Ogre::LBS_TEXTURE, Ogre::LBS_CURRENT);
            }
        }
    }
}

void applyPbrTemplate(Ogre::MaterialPtr& mat,
                      const QString& workflow)
{
    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
    if (workflow == MaterialPresetLibrary::kPbrWorkflowMetallic) {
        // Metallic-Roughness: neutral mid-gray albedo, mid shininess.
        // Real metallic/roughness response would need a custom shader;
        // for now we approximate "non-metal default" appearance.
        pass->setAmbient(Ogre::ColourValue(0.25f, 0.25f, 0.25f));
        pass->setDiffuse(Ogre::ColourValue(0.8f, 0.8f, 0.8f));
        pass->setSpecular(Ogre::ColourValue(0.5f, 0.5f, 0.5f));
        pass->setShininess(40.0f);
        pass->setLightingEnabled(true);
    } else if (workflow == MaterialPresetLibrary::kPbrWorkflowSpecular) {
        // Specular-Glossiness: brighter spec response baseline.
        pass->setAmbient(Ogre::ColourValue(0.25f, 0.25f, 0.25f));
        pass->setDiffuse(Ogre::ColourValue(0.8f, 0.8f, 0.8f));
        pass->setSpecular(Ogre::ColourValue(0.8f, 0.8f, 0.8f));
        pass->setShininess(60.0f);
        pass->setLightingEnabled(true);
    } else if (workflow == MaterialPresetLibrary::kPbrWorkflowUnlit) {
        // Unlit PBR: ignore lighting, treat albedo as final colour.
        pass->setLightingEnabled(false);
        pass->setDiffuse(Ogre::ColourValue::White);
    }
    configurePbrSlots(pass, workflow);

    // Tag the workflow on the pass so slice F can detect PBR intent
    // without name-matching the preset string. Ogre::Material doesn't
    // expose UserObjectBindings — Pass and Technique do — and the
    // pass-level tag is sufficient since PBR shading is per-pass.
    pass->getUserObjectBindings().setUserAny(
        MaterialPresetLibrary::kPbrWorkflowKey,
        Ogre::Any(Ogre::String(workflow.toStdString())));
}

struct HdrPresetConfig {
    float amb[3];
    float diff[3];
    float spec[3];
    float shininess;
    float alpha;
    float envIntensity;
    float tint[3];
    bool transparent;
};

void applyHdrPresetMaterial(Ogre::MaterialPtr& mat, const HdrPresetConfig& cfg)
{
    applyPbrTemplate(mat, MaterialPresetLibrary::kPbrWorkflowMetallic);
    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
    pass->setAmbient(Ogre::ColourValue(cfg.amb[0], cfg.amb[1], cfg.amb[2]));
    pass->setDiffuse(Ogre::ColourValue(cfg.diff[0], cfg.diff[1], cfg.diff[2], cfg.alpha));
    pass->setSpecular(Ogre::ColourValue(cfg.spec[0], cfg.spec[1], cfg.spec[2]));
    pass->setShininess(cfg.shininess);
    pass->setLightingEnabled(true);
    if (cfg.transparent) {
        pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        pass->setDepthWriteEnabled(false);
    }
    RTShaderHelper::setPbrEnvIntensity(pass, cfg.envIntensity);
    RTShaderHelper::setPbrEnvTint(
        pass, Ogre::ColourValue(cfg.tint[0], cfg.tint[1], cfg.tint[2]));
}

const HdrPresetConfig* hdrPresetConfigForName(const QString& name)
{
    static const QHash<QString, HdrPresetConfig> kConfigs = {
        {QStringLiteral("Polished Metal (HDR)"),
         {{0.18f, 0.18f, 0.18f}, {0.82f, 0.82f, 0.84f}, {1.f, 1.f, 1.f}, 90.f, 1.f, 1.35f,
          {1.f, 1.f, 1.f}, false}},
        {QStringLiteral("Brushed Metal (HDR)"),
         {{0.15f, 0.15f, 0.15f}, {0.62f, 0.62f, 0.64f}, {0.55f, 0.55f, 0.55f}, 38.f, 1.f, 1.1f,
          {1.f, 1.f, 1.f}, false}},
        {QStringLiteral("Glass (HDR)"),
         {{0.05f, 0.05f, 0.06f}, {0.72f, 0.84f, 0.92f}, {1.f, 1.f, 1.f}, 110.f, 0.32f, 1.5f,
          {1.f, 1.f, 1.f}, true}},
        {QStringLiteral("Plastic (HDR)"),
         {{0.12f, 0.08f, 0.08f}, {0.82f, 0.18f, 0.16f}, {0.45f, 0.45f, 0.45f}, 32.f, 1.f, 1.0f,
          {1.f, 1.f, 1.f}, false}},
        {QStringLiteral("Painted Wood (HDR)"),
         {{0.18f, 0.12f, 0.08f}, {0.58f, 0.36f, 0.2f}, {0.12f, 0.1f, 0.08f}, 8.f, 1.f, 0.9f,
          {1.f, 0.96f, 0.9f}, false}},
        {QStringLiteral("Skin (HDR-friendly)"),
         {{0.22f, 0.16f, 0.14f}, {0.86f, 0.62f, 0.5f}, {0.18f, 0.14f, 0.12f}, 12.f, 1.f, 0.85f,
          {1.f, 0.95f, 0.9f}, false}},
        {QStringLiteral("Car Paint (HDR)"),
         {{0.08f, 0.04f, 0.12f}, {0.18f, 0.08f, 0.55f}, {0.9f, 0.9f, 0.95f}, 98.f, 1.f, 1.45f,
          {0.95f, 0.98f, 1.f}, false}},
    };
    const auto it = kConfigs.constFind(name);
    return it == kConfigs.constEnd() ? nullptr : &(*it);
}

bool isHdrPresetName(const QString& name)
{
    return name.contains(QStringLiteral("(HDR)"))
        || name.contains(QStringLiteral("HDR-friendly"), Qt::CaseInsensitive);
}

} // namespace

QStringList MaterialPresetLibrary::presetNames() const
{
    return {"Plastic (Red)", "Plastic (Blue)", "Plastic (White)",
            "Metal (Silver)", "Metal (Gold)", "Metal (Copper)",
            "Wood (Oak)", "Wood (Birch)",
            "Glass (Clear)", "Glass (Tinted)",
            "Unlit (White)", "Wireframe",
            "Metallic-Roughness", "Specular-Glossiness", "Unlit PBR",
            "Polished Metal (HDR)", "Brushed Metal (HDR)", "Glass (HDR)",
            "Plastic (HDR)", "Painted Wood (HDR)", "Skin (HDR-friendly)",
            "Car Paint (HDR)"};
}

void MaterialPresetLibrary::applyPreset(const QString& name)
{
    auto* sel = SelectionSet::getSingleton();

    GamificationManager::noteFeature(QStringLiteral("material_editor"));

    const bool isHdrPreset = isHdrPresetName(name);
    if (isHdrPreset) {
        SentryReporter::addBreadcrumb(QStringLiteral("render.hdr.preset"), name);
        if (auto* hdrMgr = HDREnvironmentManager::getSingletonPtr()) {
            if (!hdrMgr->hasEnvironment())
                hdrMgr->loadEnvironment(QStringLiteral("studio_neutral.hdr"));
        }
    } else {
        SentryReporter::addBreadcrumb("ui.action",
            QString("Apply material preset: %1").arg(name));
    }

    auto* mgr = Ogre::MaterialManager::getSingletonPtr();
    if (!mgr) return;

    // Create or get material named after preset
    QString matName = "Preset/" + name;
    Ogre::MaterialPtr mat;
    if (mgr->resourceExists(matName.toStdString())) {
        mat = mgr->getByName(matName.toStdString());
    } else {
        mat = mgr->create(matName.toStdString(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);

        // PBR templates must be matched first: "Metallic-Roughness"
        // would otherwise hit the `name.startsWith("Metal")` branch
        // below and never run configurePbrSlots, leaving 0 TUS on the
        // pass. Specular-Glossiness and Unlit PBR are unambiguous but
        // we keep them grouped here for readability.
        if (name == "Metallic-Roughness") {
            applyPbrTemplate(mat, kPbrWorkflowMetallic);
        } else if (name == "Specular-Glossiness") {
            applyPbrTemplate(mat, kPbrWorkflowSpecular);
        } else if (name == "Unlit PBR") {
            applyPbrTemplate(mat, kPbrWorkflowUnlit);
        } else if (const HdrPresetConfig* hdrCfg = hdrPresetConfigForName(name)) {
            applyHdrPresetMaterial(mat, *hdrCfg);
        } else if (name.startsWith("Plastic")) {
            Ogre::ColourValue c(0.8f, 0.2f, 0.2f);
            if (name.contains("Blue"))  c = Ogre::ColourValue(0.2f, 0.3f, 0.9f);
            else if (name.contains("White")) c = Ogre::ColourValue(0.9f, 0.9f, 0.9f);
            pass->setAmbient(c * 0.3f);
            pass->setDiffuse(c);
            pass->setSpecular(Ogre::ColourValue(0.5f, 0.5f, 0.5f));
            pass->setShininess(30.0f);
        } else if (name.startsWith("Metal")) {
            Ogre::ColourValue c(0.8f, 0.8f, 0.8f);
            if (name.contains("Gold"))   c = Ogre::ColourValue(0.9f, 0.75f, 0.3f);
            else if (name.contains("Copper")) c = Ogre::ColourValue(0.85f, 0.5f, 0.3f);
            pass->setAmbient(c * 0.2f);
            pass->setDiffuse(c);
            pass->setSpecular(Ogre::ColourValue(1.0f, 1.0f, 1.0f));
            pass->setShininess(80.0f);
        } else if (name.startsWith("Wood")) {
            Ogre::ColourValue c(0.6f, 0.4f, 0.2f);
            if (name.contains("Birch")) c = Ogre::ColourValue(0.78f, 0.66f, 0.47f);
            pass->setAmbient(c * 0.4f);
            pass->setDiffuse(c);
            pass->setSpecular(Ogre::ColourValue(0.1f, 0.1f, 0.1f));
            pass->setShininess(5.0f);
        } else if (name.startsWith("Glass")) {
            Ogre::ColourValue c(0.6f, 0.8f, 0.9f, 0.35f);
            if (name.contains("Tinted")) c = Ogre::ColourValue(0.27f, 0.4f, 0.53f, 0.55f);
            pass->setAmbient(Ogre::ColourValue(0.1f, 0.1f, 0.1f));
            pass->setDiffuse(c);
            pass->setSpecular(Ogre::ColourValue(1.0f, 1.0f, 1.0f));
            pass->setShininess(100.0f);
            pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
            pass->setDepthWriteEnabled(false);
        } else if (name == "Unlit (White)") {
            pass->setLightingEnabled(false);
            pass->setDiffuse(Ogre::ColourValue::White);
        } else if (name == "Wireframe") {
            pass->setPolygonMode(Ogre::PM_WIREFRAME);
            pass->setLightingEnabled(false);
            pass->setDiffuse(Ogre::ColourValue(0.6f, 0.9f, 0.6f));
        }
        // PBR templates are dispatched at the top of this branch — see
        // the comment there for why they must be matched before the
        // Plastic/Metal/etc startsWith() checks.

        // autoManageTextureUnits=false: keep all 6 PBR slots on a single
        // pass. With the default `true`, Ogre splits the pass when the
        // render system's max-texture-units cap is below the slot count
        // (e.g., Mesa software in CI reports 8 but auto-manage still
        // re-shuffles), which causes getTechnique(0)->getPass(0) to lose
        // slots. Tests + slice F shaders need the slot count preserved.
        mat->compile(/*autoManageTextureUnits=*/false);

        // Slice F: if the just-built material is tagged with the
        // metallic_roughness PBR workflow, attach Ogre's stock
        // SRS_COOK_TORRANCE_LIGHTING SRS so it renders with a real
        // Cook-Torrance BRDF instead of the slice E FFP approximation.
        // applyPbrIfTagged returns false (and the material falls back
        // to FFP) for other workflows or when SRS_COOK_TORRANCE_LIGHTING
        // isn't built into the running Ogre.
        RTShaderHelper::applyPbrIfTagged(mat);
    }

    // Apply to resolved entities (handles node selection as well as direct entity/sub-entity selection)
    std::string stdMatName = matName.toStdString();
    auto resolvedEntities = sel->getResolvedEntities();
    auto subEntities = sel->getSubEntitiesSelectionList();

    if (resolvedEntities.isEmpty() && subEntities.isEmpty())
        return;

    // Snapshot ALL sub-entity materials before applying, so undo can restore
    // entities with mixed per-sub-entity materials correctly.
    QList<MaterialPresetCommand::EntityMaterial> entMats;
    QList<MaterialPresetCommand::SubEntityMaterial> subMats;

    for (Ogre::Entity* ent : resolvedEntities) {
        MaterialPresetCommand::EntityMaterial em;
        em.entity = ent;
        em.newMaterialName = stdMatName;
        entMats.append(em);

        for (unsigned int i = 0; i < ent->getNumSubEntities(); ++i) {
            MaterialPresetCommand::SubEntityMaterial sm;
            sm.subEntity = ent->getSubEntity(i);
            sm.oldMaterialName = sm.subEntity->getMaterialName();
            sm.newMaterialName = stdMatName;
            subMats.append(sm);
        }
    }

    for (Ogre::SubEntity* sub : subEntities) {
        MaterialPresetCommand::SubEntityMaterial sm;
        sm.subEntity = sub;
        sm.oldMaterialName = sub->getMaterialName();
        sm.newMaterialName = stdMatName;
        subMats.append(sm);
    }

    // Apply after full snapshot is captured
    for (Ogre::Entity* ent : resolvedEntities)
        ent->setMaterialName(stdMatName);
    for (Ogre::SubEntity* sub : subEntities)
        sub->setMaterialName(stdMatName);

    UndoManager::getSingleton()->push(
        new MaterialPresetCommand(entMats, subMats, name));

    emit presetApplied(name);
}
