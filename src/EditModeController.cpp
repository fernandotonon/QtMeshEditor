/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------------
*/

#include "EditModeController.h"
#include "EditableMesh.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include <Ogre.h>

EditModeController* EditModeController::m_pSingleton = nullptr;

EditModeController::EditModeController()
    : QObject(nullptr)
{
    // Track selection changes to update canEnterEditMode and auto-exit
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, &EditModeController::onSelectionChanged);
}

EditModeController::~EditModeController()
{
    // Exit cleanly if still in edit mode
    if (m_editModeActive)
        exitEditMode(false);
}

EditModeController* EditModeController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new EditModeController();
    return m_pSingleton;
}

EditModeController* EditModeController::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine);
    Q_UNUSED(scriptEngine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void EditModeController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

QString EditModeController::modeLabel() const
{
    return m_editModeActive ? QStringLiteral("Edit Mode") : QStringLiteral("Object Mode");
}

bool EditModeController::canEnterEditMode() const
{
    // Edit mode requires exactly one entity selected (either directly or via node)
    auto* sel = SelectionSet::getSingleton();
    QList<Ogre::Entity*> entities = sel->getResolvedEntities();
    return entities.size() == 1;
}

int EditModeController::vertexCount() const
{
    if (!m_editableMesh) return 0;
    return static_cast<int>(m_editableMesh->totalVertexCount());
}

int EditModeController::triangleCount() const
{
    if (!m_editableMesh) return 0;
    return static_cast<int>(m_editableMesh->totalTriangleCount());
}

int EditModeController::subMeshCount() const
{
    if (!m_editableMesh) return 0;
    return static_cast<int>(m_editableMesh->subMeshCount());
}

void EditModeController::toggleEditMode()
{
    if (m_editModeActive)
        exitEditMode(true);
    else
        enterEditMode();
}

bool EditModeController::enterEditMode()
{
    if (m_editModeActive)
        return true; // Already in edit mode

    if (!canEnterEditMode()) {
        SentryReporter::addBreadcrumb("edit_mode", "Cannot enter Edit Mode: no single entity selected");
        return false;
    }

    auto* sel = SelectionSet::getSingleton();
    QList<Ogre::Entity*> entities = sel->getResolvedEntities();
    m_editEntity = entities.first();

    // Decompose mesh into editable data
    m_editableMesh = std::make_unique<EditableMesh>();
    if (!m_editableMesh->loadFromEntity(m_editEntity)) {
        SentryReporter::addBreadcrumb("edit_mode", "Failed to load mesh data for Edit Mode");
        m_editableMesh.reset();
        m_editEntity = nullptr;
        return false;
    }

    m_editModeActive = true;

    SentryReporter::addBreadcrumb("edit_mode",
        QString("Entered Edit Mode: %1 vertices, %2 triangles, %3 submeshes")
            .arg(m_editableMesh->totalVertexCount())
            .arg(m_editableMesh->totalTriangleCount())
            .arg(m_editableMesh->subMeshCount()));

    emit editModeChanged();
    emit meshDataChanged();

    return true;
}

void EditModeController::exitEditMode(bool commitChanges)
{
    if (!m_editModeActive)
        return;

    if (commitChanges && m_editableMesh && m_editEntity) {
        m_editableMesh->commitToEntity(m_editEntity);
        SentryReporter::addBreadcrumb("edit_mode", "Exited Edit Mode: changes committed");
    } else {
        SentryReporter::addBreadcrumb("edit_mode", "Exited Edit Mode: changes discarded");
    }

    m_editableMesh.reset();
    m_editEntity = nullptr;
    m_editModeActive = false;

    emit editModeChanged();
    emit meshDataChanged();
}

void EditModeController::onSelectionChanged()
{
    // If selection changes while in edit mode and the edited entity is no
    // longer selected, auto-exit edit mode (committing changes).
    if (m_editModeActive && m_editEntity) {
        auto* sel = SelectionSet::getSingleton();
        QList<Ogre::Entity*> entities = sel->getResolvedEntities();
        bool stillSelected = entities.contains(m_editEntity);
        if (!stillSelected) {
            exitEditMode(true);
        }
    }

    emit selectionStateChanged();
}
