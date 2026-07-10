#include "SkinningDisplay.h"

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSubEntity.h>
#include <OgreShaderGenerator.h>
#include <OgreShaderExHardwareSkinning.h>

#include <set>
#include <string>

namespace {

// Key under which we track the applied mode on the ENTITY.
const char* kModeBindKey = "qtme.skinning.display";

// The user-binding key Ogre's HardwareSkinningFactory imprints the
// per-material SkinningData under. The constant lives file-local in
// OgreShaderExHardwareSkinning.cpp ("HS_SRS_DATA", unchanged since
// Ogre 1.8) — mirrored here so Linear mode can erase the imprint
// and return the material to the default path.
const char* kOgreHsDataKey = "HS_SRS_DATA";

// Unique materials across the entity's subentities.
std::set<Ogre::Material*> entityMaterials(const Ogre::Entity* entity)
{
    std::set<Ogre::Material*> mats;
    for (size_t i = 0; i < entity->getNumSubEntities(); ++i) {
        const auto& mat = entity->getSubEntity(i)->getMaterial();
        if (mat) mats.insert(mat.get());
    }
    return mats;
}

} // namespace

bool SkinningDisplay::apply(Ogre::Entity* entity, Mode mode, QString* error)
{
    auto fail = [&](const QString& msg) {
        if (error) *error = msg;
        return false;
    };
    if (!entity || !entity->getMesh())
        return fail(QStringLiteral("no entity"));
    if (!entity->getMesh()->getSkeleton())
        return fail(QStringLiteral("mesh has no skeleton attached"));

    auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    auto* hsFactory = Ogre::RTShader::HardwareSkinningFactory::getSingletonPtr();
    if (!shaderGen || !hsFactory)
        return fail(QStringLiteral("RTSS is not initialized"));

    if (mode == Mode::DualQuaternion) {
        // Imprints every material of the entity with the skeleton's
        // bone/weight counts + the DQS flag; the hardware-skinning
        // SRS picks that up when the technique regenerates.
        // correctAntipodality=true avoids the flipped-quaternion
        // seam on twist poses (the reason to use DQS at all).
        Ogre::RTShader::HardwareSkinningFactory::prepareEntityForSkinning(
            entity, Ogre::RTShader::ST_DUAL_QUATERNION,
            /*correctAntidpodalityHandling*/ true, /*shearScale*/ false);
    } else {
        // Back to the default path: erase the imprint so the SRS
        // deactivates on the regenerated technique.
        for (Ogre::Material* mat : entityMaterials(entity)) {
            if (mat->getNumTechniques() == 0
                || mat->getTechnique(0)->getNumPasses() == 0)
                continue;
            mat->getTechnique(0)->getPass(0)
               ->getUserObjectBindings().eraseUserAny(kOgreHsDataKey);
        }
    }

    // Rebuild the generated techniques of every affected material.
    for (Ogre::Material* mat : entityMaterials(entity)) {
        shaderGen->invalidateMaterial(Ogre::MSN_SHADERGEN,
                                      mat->getName(), mat->getGroup());
    }

    entity->getUserObjectBindings().setUserAny(
        kModeBindKey, Ogre::Any(static_cast<int>(mode)));
    return true;
}

SkinningDisplay::Mode SkinningDisplay::current(const Ogre::Entity* entity)
{
    if (!entity) return Mode::Linear;
    const Ogre::Any& any =
        entity->getUserObjectBindings().getUserAny(kModeBindKey);
    if (!any.has_value()) return Mode::Linear;
    const int* v = Ogre::any_cast<int>(&any);
    return (v && *v == static_cast<int>(Mode::DualQuaternion))
        ? Mode::DualQuaternion : Mode::Linear;
}

QString SkinningDisplay::modeToString(Mode mode)
{
    return mode == Mode::DualQuaternion ? QStringLiteral("dual-quaternion")
                                        : QStringLiteral("linear");
}

SkinningDisplay::Mode SkinningDisplay::modeFromString(const QString& s)
{
    const QString v = s.trimmed().toLower();
    if (v == QLatin1String("dual-quaternion") || v == QLatin1String("dqs")
        || v == QLatin1String("dual_quaternion"))
        return Mode::DualQuaternion;
    return Mode::Linear;
}
