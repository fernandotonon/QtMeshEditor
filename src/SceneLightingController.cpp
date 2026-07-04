#include "SceneLightingController.h"

#include "HDR/HdrEnvironmentController.h"
#include "LightManager.h"
#include "LightRigLibrary.h"
#include "Manager.h"
#include "UndoManager.h"
#include "commands/LightCommands.h"
#include "mainwindow.h"

#include <QMessageBox>
#include <QWidget>

#include <QFileInfo>

SceneLightingController* SceneLightingController::s_singleton = nullptr;

SceneLightingController* SceneLightingController::instance()
{
    if (!s_singleton)
        s_singleton = new SceneLightingController(); // NOSONAR — singleton
    return s_singleton;
}

SceneLightingController* SceneLightingController::qmlInstance(QQmlEngine*, QJSEngine*)
{
    return instance();
}

void SceneLightingController::kill()
{
    delete s_singleton; // NOSONAR — singleton
    s_singleton = nullptr;
}

SceneLightingController::SceneLightingController(QObject* parent)
    : QObject(parent)
{
    const QString defaultId = LightRigLibrary::readDefaultRigId();
    m_selectedRigIndex = LightRigLibrary::indexOfRig(defaultId);
    if (m_selectedRigIndex < 0)
        m_selectedRigIndex = 0;
}

QStringList SceneLightingController::rigIds() const
{
    return LightRigLibrary::rigIds();
}

QStringList SceneLightingController::rigNames() const
{
    QStringList names;
    for (const QString& id : LightRigLibrary::rigIds())
        names.append(LightRigLibrary::displayNameForId(id));
    return names;
}

int SceneLightingController::selectedRigIndex() const
{
    return m_selectedRigIndex;
}

void SceneLightingController::setSelectedRigIndex(int index)
{
    if (index < 0 || index >= rigIds().size())
        return;
    if (m_selectedRigIndex == index)
        return;
    m_selectedRigIndex = index;
    emit selectedRigIndexChanged();
}

bool SceneLightingController::replaceExistingLights() const
{
    return m_replaceExistingLights;
}

void SceneLightingController::setReplaceExistingLights(bool replace)
{
    if (m_replaceExistingLights == replace)
        return;
    m_replaceExistingLights = replace;
    emit replaceExistingLightsChanged();
}

bool SceneLightingController::confirmReplaceExisting(QWidget* parent) const
{
    const QMessageBox::StandardButton answer = QMessageBox::question(
        parent,
        QObject::tr("Replace existing lights?"),
        QObject::tr("Apply the preset rig and remove all current user lights?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);
    return answer == QMessageBox::Yes;
}

void SceneLightingController::maybeLoadSuggestedHdri(const QString& hdriName)
{
    if (hdriName.isEmpty())
        return;

    auto* hdr = HdrEnvironmentController::instance();
    if (!hdr)
        return;

    const QString current = hdr->currentEnvironment();
    const bool alreadyLoaded =
        !current.isEmpty()
        && QFileInfo(current).fileName().compare(hdriName, Qt::CaseInsensitive) == 0;

    if (!alreadyLoaded)
        hdr->loadEnvironment(hdriName);

    if (hdr->hasEnvironment())
        hdr->setDefaultSkyBoxVisible(true);
}

void SceneLightingController::applyRig(const QString& rigId, bool replaceExisting)
{
    auto* lights = LightManager::getSingleton();
    if (!lights)
        return;

    QWidget* parent = Manager::getSingletonPtr() ? Manager::getSingleton()->getMainWindow() : nullptr;
    if (replaceExisting && !lights->lights().isEmpty()
        && !confirmReplaceExisting(parent))
    {
        return;
    }

    const LightRigApplyResult result = LightRigLibrary::apply(rigId, replaceExisting);
    if (!result.ok)
    {
        if (!result.error.isEmpty())
        {
            QMessageBox::warning(parent,
                                 QObject::tr("Light rig"),
                                 result.error);
        }
        return;
    }

    UndoManager::getSingleton()->push(new ApplyLightRigCommand(result));
    maybeLoadSuggestedHdri(result.suggestedHdri);
}

void SceneLightingController::applySelectedRig()
{
    const QStringList ids = rigIds();
    if (m_selectedRigIndex < 0 || m_selectedRigIndex >= ids.size())
        return;
    applyRig(ids.at(m_selectedRigIndex), m_replaceExistingLights);
}
