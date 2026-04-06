#include "MaterialPresetLibrary.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
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

QStringList MaterialPresetLibrary::presetNames() const
{
    return {"Plastic (Red)", "Plastic (Blue)", "Plastic (White)",
            "Metal (Silver)", "Metal (Gold)", "Metal (Copper)",
            "Wood (Oak)", "Wood (Birch)",
            "Glass (Clear)", "Glass (Tinted)",
            "Unlit (White)", "Wireframe"};
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
        }

        mat->compile();
    }

    // Apply to resolved entities (handles node selection as well as direct entity/sub-entity selection)
    std::string stdMatName = matName.toStdString();
    auto resolvedEntities = sel->getResolvedEntities();
    auto subEntities = sel->getSubEntitiesSelectionList();

    if (resolvedEntities.isEmpty() && subEntities.isEmpty())
        return;

    for (Ogre::Entity* ent : resolvedEntities)
        ent->setMaterialName(stdMatName);
    for (Ogre::SubEntity* sub : subEntities)
        sub->setMaterialName(stdMatName);

    emit presetApplied(name);
}
