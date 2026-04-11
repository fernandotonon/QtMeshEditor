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

#ifndef EDITMODECONTROLLER_H
#define EDITMODECONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <memory>

class EditableMesh;

namespace Ogre { class Entity; }

/**
 * @brief QML_SINGLETON that manages Object Mode / Edit Mode state.
 *
 * In Object Mode, the user manipulates entire scene nodes (translate,
 * rotate, scale). In Edit Mode, the user can manipulate individual
 * vertices, edges, and faces of a single mesh entity.
 *
 * Entering Edit Mode decomposes the selected entity's mesh into an
 * editable representation (EditableMesh). Exiting Edit Mode commits
 * the changes back to the Ogre buffers and recalculates normals/bounds.
 *
 * The Tab key toggles between modes.
 */
class EditModeController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool editModeActive READ isEditModeActive NOTIFY editModeChanged)
    Q_PROPERTY(QString modeLabel READ modeLabel NOTIFY editModeChanged)
    Q_PROPERTY(bool canEnterEditMode READ canEnterEditMode NOTIFY selectionStateChanged)
    Q_PROPERTY(int vertexCount READ vertexCount NOTIFY meshDataChanged)
    Q_PROPERTY(int triangleCount READ triangleCount NOTIFY meshDataChanged)
    Q_PROPERTY(int subMeshCount READ subMeshCount NOTIFY meshDataChanged)

public:
    static EditModeController* instance();
    static EditModeController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    /// @name Mode state
    /// @{
    bool isEditModeActive() const { return m_editModeActive; }
    QString modeLabel() const;

    /// Returns true if the current selection allows entering edit mode
    /// (exactly one mesh entity selected).
    bool canEnterEditMode() const;
    /// @}

    /// @name Mesh info (only valid when in edit mode)
    /// @{
    int vertexCount() const;
    int triangleCount() const;
    int subMeshCount() const;
    /// @}

    /// @name Mode transitions
    /// @{
    Q_INVOKABLE void toggleEditMode();
    Q_INVOKABLE bool enterEditMode();
    Q_INVOKABLE void exitEditMode(bool commitChanges = true);
    /// @}

    /// Access the current editable mesh (nullptr if not in edit mode).
    EditableMesh* currentMesh() const { return m_editableMesh.get(); }

    /// Returns the entity being edited (nullptr if not in edit mode).
    Ogre::Entity* editEntity() const { return m_editEntity; }

signals:
    /// Emitted when entering or exiting edit mode.
    void editModeChanged();
    /// Emitted when the mesh data is modified during edit mode.
    void meshDataChanged();
    /// Emitted when the selection changes (to update canEnterEditMode).
    void selectionStateChanged();

private slots:
    void onSelectionChanged();

private:
    EditModeController();
    ~EditModeController() override;

    static EditModeController* m_pSingleton;

    bool m_editModeActive = false;
    std::unique_ptr<EditableMesh> m_editableMesh;
    Ogre::Entity* m_editEntity = nullptr;
};

#endif // EDITMODECONTROLLER_H
