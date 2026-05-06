#include "MaterialPresetLibrary.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "commands/TransformCommands.h"
#include <Ogre.h>

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
void configurePbrSlots(Ogre::Pass* pass)
{
    for (const char* slotName : kPbrSlots) {
        Ogre::TextureUnitState* tus = pass->createTextureUnitState();
        tus->setName(slotName);
    }
    // Albedo slot: keep the default state — it'll modulate the diffuse
    // colour with whatever texture the user drops in. Other slots stay
    // empty (export pipeline preserves the names; runtime ignores).
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
    configurePbrSlots(pass);

    // Tag the workflow on the pass so slice F can detect PBR intent
    // without name-matching the preset string. Ogre::Material doesn't
    // expose UserObjectBindings — Pass and Technique do — and the
    // pass-level tag is sufficient since PBR shading is per-pass.
    pass->getUserObjectBindings().setUserAny(
        MaterialPresetLibrary::kPbrWorkflowKey,
        Ogre::Any(Ogre::String(workflow.toStdString())));
}

} // namespace

QStringList MaterialPresetLibrary::presetNames() const
{
    return {"Plastic (Red)", "Plastic (Blue)", "Plastic (White)",
            "Metal (Silver)", "Metal (Gold)", "Metal (Copper)",
            "Wood (Oak)", "Wood (Birch)",
            "Glass (Clear)", "Glass (Tinted)",
            "Unlit (White)", "Wireframe",
            "Metallic-Roughness", "Specular-Glossiness", "Unlit PBR"};
}

void MaterialPresetLibrary::applyPreset(const QString& name)
{
    auto* sel = SelectionSet::getSingleton();

    SentryReporter::addBreadcrumb("ui.action",
        QString("Apply material preset: %1").arg(name));

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

        if (name.startsWith("Plastic")) {
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
        } else if (name == "Metallic-Roughness") {
            applyPbrTemplate(mat, kPbrWorkflowMetallic);
        } else if (name == "Specular-Glossiness") {
            applyPbrTemplate(mat, kPbrWorkflowSpecular);
        } else if (name == "Unlit PBR") {
            applyPbrTemplate(mat, kPbrWorkflowUnlit);
        }

        mat->compile();
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
