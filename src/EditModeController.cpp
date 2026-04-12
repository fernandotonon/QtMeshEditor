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
#include "Manager.h"
#include "NormalVisualizer.h"
#include "mainwindow.h"
#include "OgreWidget.h"
#include "SpaceCamera.h"
#include <Ogre.h>
#include <ProceduralSphereGenerator.h>
#include <cmath>
#include <algorithm>
#include <limits>

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
    if (!m_editModeActive)
        return QStringLiteral("Object Mode");

    switch (m_selectionMode) {
    case VertexMode: return QStringLiteral("Edit Mode (Vertex)");
    case EdgeMode:   return QStringLiteral("Edit Mode (Edge)");
    case FaceMode:   return QStringLiteral("Edit Mode (Face)");
    }
    return QStringLiteral("Edit Mode");
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

    // Reset selection state
    m_selectionMode = VertexMode;
    m_selectedVertices.clear();
    m_selectedEdges.clear();
    m_selectedFaces.clear();

    // Create overlay materials and initial (empty) overlays
    createOverlayMaterials();

    // Re-apply wireframe if it was enabled in a previous session
    if (m_wireframeEnabled)
        applyWireframeMaterials();

    SentryReporter::addBreadcrumb("edit_mode",
        QString("Entered Edit Mode: %1 vertices, %2 triangles, %3 submeshes")
            .arg(m_editableMesh->totalVertexCount())
            .arg(m_editableMesh->totalTriangleCount())
            .arg(m_editableMesh->subMeshCount()));

    // Run initial validation
    validateMesh();

    emit editModeChanged();
    emit meshDataChanged();
    emit selectionModeChanged();
    emit editSelectionChanged();

    return true;
}

void EditModeController::exitEditMode(bool commitChanges)
{
    if (!m_editModeActive)
        return;

    if (commitChanges && m_editableMesh && m_editEntity) {
        bool ok = m_editableMesh->commitToEntity(m_editEntity);
        refreshNormalVisualizer();
        SentryReporter::addBreadcrumb("edit_mode",
            ok ? "Exited Edit Mode: changes committed"
               : "Exited Edit Mode: commit failed");
    } else {
        SentryReporter::addBreadcrumb("edit_mode", "Exited Edit Mode: changes discarded");
    }

    // Restore original materials (keep m_wireframeEnabled so it persists)
    if (!m_savedMaterials.empty())
        removeWireframeMaterials();

    // Clean up selection overlays
    destroySelectionOverlay();

    // Clear selection state
    m_selectedVertices.clear();
    m_selectedEdges.clear();
    m_selectedFaces.clear();

    m_editableMesh.reset();
    m_editEntity = nullptr;
    m_editModeActive = false;

    // Reset validation state for next session
    m_degenerateTriangleCount = 0;

    emit editModeChanged();
    emit meshDataChanged();
    emit editSelectionChanged();
    emit validationChanged();
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

// ===========================================================================
// Selection mode
// ===========================================================================

void EditModeController::setSelectionMode(int mode)
{
    if (mode < 0 || mode > 2)
        return;

    auto newMode = static_cast<SelectionMode>(mode);
    if (m_selectionMode == newMode)
        return;

    m_selectionMode = newMode;

    // Clear selection from previous mode
    m_selectedVertices.clear();
    m_selectedEdges.clear();
    m_selectedFaces.clear();

    QString modeName;
    switch (m_selectionMode) {
    case VertexMode: modeName = "Vertex"; break;
    case EdgeMode:   modeName = "Edge"; break;
    case FaceMode:   modeName = "Face"; break;
    }
    SentryReporter::addBreadcrumb("edit_mode",
        QString("Selection mode changed to %1").arg(modeName));

    // Update overlay to show the correct selection type
    updateSelectionOverlay();

    emit selectionModeChanged();
    emit editModeChanged(); // modeLabel changed
}

// ===========================================================================
// Selection operations
// ===========================================================================

void EditModeController::selectAll()
{
    if (!m_editModeActive || !m_editableMesh)
        return;

    SentryReporter::addBreadcrumb("edit_mode", "Select All");

    int totalVerts = static_cast<int>(m_editableMesh->totalVertexCount());
    int totalTris = static_cast<int>(m_editableMesh->totalTriangleCount());

    // Select all vertices
    m_selectedVertices.clear();
    for (int i = 0; i < totalVerts; ++i)
        m_selectedVertices.insert(i);

    // Select all edges (from all triangles)
    m_selectedEdges.clear();
    const auto& subMeshes = m_editableMesh->subMeshes();
    int globalTriOffset = 0;
    for (size_t si = 0; si < subMeshes.size(); ++si) {
        int vertexOffset = localToGlobal(si, 0);
        for (const auto& tri : subMeshes[si].triangles) {
            int g0 = vertexOffset + static_cast<int>(tri.indices[0]);
            int g1 = vertexOffset + static_cast<int>(tri.indices[1]);
            int g2 = vertexOffset + static_cast<int>(tri.indices[2]);
            m_selectedEdges.insert({std::min(g0, g1), std::max(g0, g1)});
            m_selectedEdges.insert({std::min(g1, g2), std::max(g1, g2)});
            m_selectedEdges.insert({std::min(g0, g2), std::max(g0, g2)});
        }
        globalTriOffset += static_cast<int>(subMeshes[si].triangles.size());
    }

    // Select all faces
    m_selectedFaces.clear();
    for (int i = 0; i < totalTris; ++i)
        m_selectedFaces.insert(i);

    updateSelectionOverlay();
    emit editSelectionChanged();
}

void EditModeController::deselectAll()
{
    if (!m_editModeActive)
        return;

    SentryReporter::addBreadcrumb("edit_mode", "Deselect All");

    m_selectedVertices.clear();
    m_selectedEdges.clear();
    m_selectedFaces.clear();

    updateSelectionOverlay();
    emit editSelectionChanged();
}

void EditModeController::selectVertex(int globalIndex, bool addToSelection)
{
    if (!m_editModeActive || !m_editableMesh)
        return;

    if (!addToSelection)
        m_selectedVertices.clear();

    if (globalIndex < 0 || globalIndex >= static_cast<int>(m_editableMesh->totalVertexCount()))
        return;

    m_selectedVertices.insert(globalIndex);
    updateSelectionOverlay();
    emit editSelectionChanged();
}

void EditModeController::deselectVertex(int globalIndex)
{
    if (m_selectedVertices.erase(globalIndex) > 0) {
        updateSelectionOverlay();
        emit editSelectionChanged();
    }
}

void EditModeController::selectEdge(int v1, int v2, bool addToSelection)
{
    if (!m_editModeActive || !m_editableMesh)
        return;

    auto edge = std::make_pair(std::min(v1, v2), std::max(v1, v2));

    if (!addToSelection) {
        m_selectedEdges.clear();
        m_selectedVertices.clear();
    }

    m_selectedEdges.insert(edge);
    // Also select the endpoint vertices
    m_selectedVertices.insert(edge.first);
    m_selectedVertices.insert(edge.second);

    updateSelectionOverlay();
    emit editSelectionChanged();
}

void EditModeController::deselectEdge(int v1, int v2)
{
    auto edge = std::make_pair(std::min(v1, v2), std::max(v1, v2));
    if (m_selectedEdges.erase(edge) > 0) {
        // Note: we don't remove vertices here as they may be part of other selected edges
        updateSelectionOverlay();
        emit editSelectionChanged();
    }
}

void EditModeController::selectFace(int triIndex, bool addToSelection)
{
    if (!m_editModeActive || !m_editableMesh)
        return;
    if (triIndex < 0 || triIndex >= static_cast<int>(m_editableMesh->totalTriangleCount()))
        return;

    if (!addToSelection) {
        m_selectedFaces.clear();
        m_selectedVertices.clear();
        m_selectedEdges.clear();
    }

    m_selectedFaces.insert(triIndex);

    // Also select the three vertices and three edges of the triangle
    auto [subIdx, localTri] = globalTriToLocal(triIndex);
    if (subIdx < m_editableMesh->subMeshes().size()) {
        const auto& tri = m_editableMesh->subMeshes()[subIdx].triangles[localTri];
        int vertOffset = localToGlobal(subIdx, 0);
        int g0 = vertOffset + static_cast<int>(tri.indices[0]);
        int g1 = vertOffset + static_cast<int>(tri.indices[1]);
        int g2 = vertOffset + static_cast<int>(tri.indices[2]);

        m_selectedVertices.insert(g0);
        m_selectedVertices.insert(g1);
        m_selectedVertices.insert(g2);

        m_selectedEdges.insert({std::min(g0, g1), std::max(g0, g1)});
        m_selectedEdges.insert({std::min(g1, g2), std::max(g1, g2)});
        m_selectedEdges.insert({std::min(g0, g2), std::max(g0, g2)});
    }

    updateSelectionOverlay();
    emit editSelectionChanged();
}

void EditModeController::deselectFace(int triIndex)
{
    if (m_selectedFaces.erase(triIndex) > 0) {
        updateSelectionOverlay();
        emit editSelectionChanged();
    }
}

// ===========================================================================
// Index conversion helpers
// ===========================================================================

std::pair<size_t, size_t> EditModeController::globalToLocal(int globalIndex) const
{
    if (!m_editableMesh)
        return {0, 0};

    int offset = 0;
    for (size_t si = 0; si < m_editableMesh->subMeshes().size(); ++si) {
        int subCount = static_cast<int>(m_editableMesh->subMeshes()[si].vertices.size());
        if (globalIndex < offset + subCount)
            return {si, static_cast<size_t>(globalIndex - offset)};
        offset += subCount;
    }
    return {0, 0};
}

int EditModeController::localToGlobal(size_t subMeshIndex, size_t localVertexIndex) const
{
    if (!m_editableMesh)
        return 0;

    int offset = 0;
    for (size_t si = 0; si < subMeshIndex && si < m_editableMesh->subMeshes().size(); ++si)
        offset += static_cast<int>(m_editableMesh->subMeshes()[si].vertices.size());

    return offset + static_cast<int>(localVertexIndex);
}

std::pair<size_t, size_t> EditModeController::globalTriToLocal(int globalTriIndex) const
{
    if (!m_editableMesh)
        return {0, 0};

    int offset = 0;
    for (size_t si = 0; si < m_editableMesh->subMeshes().size(); ++si) {
        int subCount = static_cast<int>(m_editableMesh->subMeshes()[si].triangles.size());
        if (globalTriIndex < offset + subCount)
            return {si, static_cast<size_t>(globalTriIndex - offset)};
        offset += subCount;
    }
    return {0, 0};
}

int EditModeController::localTriToGlobal(size_t subMeshIndex, size_t localTriIndex) const
{
    if (!m_editableMesh)
        return 0;

    int offset = 0;
    for (size_t si = 0; si < subMeshIndex && si < m_editableMesh->subMeshes().size(); ++si)
        offset += static_cast<int>(m_editableMesh->subMeshes()[si].triangles.size());

    return offset + static_cast<int>(localTriIndex);
}

// ===========================================================================
// Geometry helpers
// ===========================================================================

QPoint EditModeController::worldToScreen(const Ogre::Vector3& worldPos,
                                          Ogre::Camera* camera,
                                          int viewportWidth, int viewportHeight)
{
    if (!camera)
        return QPoint(-1, -1);

    Ogre::Vector3 screenPos = camera->getProjectionMatrix() * camera->getViewMatrix() * worldPos;

    // Convert from NDC (-1..1) to pixel coordinates
    int x = static_cast<int>((screenPos.x * 0.5f + 0.5f) * viewportWidth);
    int y = static_cast<int>((1.0f - (screenPos.y * 0.5f + 0.5f)) * viewportHeight);

    return QPoint(x, y);
}

float EditModeController::pointToSegmentDistance(const QPoint& point,
                                                  const QPoint& segA,
                                                  const QPoint& segB)
{
    float px = static_cast<float>(point.x());
    float py = static_cast<float>(point.y());
    float ax = static_cast<float>(segA.x());
    float ay = static_cast<float>(segA.y());
    float bx = static_cast<float>(segB.x());
    float by = static_cast<float>(segB.y());

    float dx = bx - ax;
    float dy = by - ay;
    float lenSq = dx * dx + dy * dy;

    if (lenSq < 1e-8f) {
        // Degenerate segment (both endpoints are the same)
        float ddx = px - ax;
        float ddy = py - ay;
        return std::sqrt(ddx * ddx + ddy * ddy);
    }

    // Parameter t of closest point on the line
    float t = ((px - ax) * dx + (py - ay) * dy) / lenSq;
    t = std::max(0.0f, std::min(1.0f, t));

    float closestX = ax + t * dx;
    float closestY = ay + t * dy;

    float ddx = px - closestX;
    float ddy = py - closestY;
    return std::sqrt(ddx * ddx + ddy * ddy);
}

float EditModeController::rayTriangleIntersect(const Ogre::Vector3& rayOrigin,
                                                const Ogre::Vector3& rayDir,
                                                const Ogre::Vector3& v0,
                                                const Ogre::Vector3& v1,
                                                const Ogre::Vector3& v2)
{
    const float EPSILON = 1e-7f;

    Ogre::Vector3 edge1 = v1 - v0;
    Ogre::Vector3 edge2 = v2 - v0;
    Ogre::Vector3 h = rayDir.crossProduct(edge2);
    float a = edge1.dotProduct(h);

    if (a > -EPSILON && a < EPSILON)
        return -1.0f; // Ray is parallel to the triangle

    float f = 1.0f / a;
    Ogre::Vector3 s = rayOrigin - v0;
    float u = f * s.dotProduct(h);

    if (u < 0.0f || u > 1.0f)
        return -1.0f;

    Ogre::Vector3 q = s.crossProduct(edge1);
    float v = f * rayDir.dotProduct(q);

    if (v < 0.0f || u + v > 1.0f)
        return -1.0f;

    float t = f * edge2.dotProduct(q);

    if (t > EPSILON)
        return t;

    return -1.0f;
}

// ===========================================================================
// Hit testing
// ===========================================================================

int EditModeController::hitTestVertex(const QPoint& screenPos,
                                       Ogre::Camera* camera,
                                       int viewportWidth, int viewportHeight,
                                       float pixelRadius) const
{
    if (!m_editableMesh || !m_editEntity || !camera)
        return -1;

    Ogre::SceneNode* node = m_editEntity->getParentSceneNode();
    if (!node)
        return -1;

    float bestDistSq = pixelRadius * pixelRadius;
    int bestIndex = -1;
    int globalOffset = 0;

    for (size_t si = 0; si < m_editableMesh->subMeshes().size(); ++si) {
        const auto& sub = m_editableMesh->subMeshes()[si];
        for (size_t vi = 0; vi < sub.vertices.size(); ++vi) {
            int globalIdx = globalOffset + static_cast<int>(vi);

            // Transform from local space to world space
            Ogre::Vector3 worldPos = node->convertLocalToWorldPosition(sub.vertices[vi].position);

            // Check if the vertex is behind the camera
            Ogre::Vector3 camToVert = worldPos - camera->getDerivedPosition();
            if (camToVert.dotProduct(camera->getDerivedDirection()) < 0)
                continue;

            QPoint sp = worldToScreen(worldPos, camera, viewportWidth, viewportHeight);
            float dx = static_cast<float>(sp.x() - screenPos.x());
            float dy = static_cast<float>(sp.y() - screenPos.y());
            float distSq = dx * dx + dy * dy;

            if (distSq < bestDistSq) {
                bestDistSq = distSq;
                bestIndex = globalIdx;
            }
        }
        globalOffset += static_cast<int>(sub.vertices.size());
    }

    return bestIndex;
}

// LCOV_EXCL_START — requires active render window + camera for ray casting
int EditModeController::hitTestFace(const QPoint& screenPos,
                                     Ogre::Camera* camera,
                                     int viewportWidth, int viewportHeight) const
{
    if (!m_editableMesh || !m_editEntity || !camera)
        return -1;

    Ogre::SceneNode* node = m_editEntity->getParentSceneNode();
    if (!node)
        return -1;

    // Cast a ray from the camera through the click point
    Ogre::Real nx = static_cast<Ogre::Real>(screenPos.x()) / static_cast<Ogre::Real>(viewportWidth);
    Ogre::Real ny = static_cast<Ogre::Real>(screenPos.y()) / static_cast<Ogre::Real>(viewportHeight);
    Ogre::Ray ray = camera->getCameraToViewportRay(nx, ny);

    // Transform ray into entity's local space
    Ogre::Affine3 worldToLocal = node->_getFullTransform().inverse();
    Ogre::Vector3 localOrigin = worldToLocal * ray.getOrigin();
    Ogre::Vector3 localDir = worldToLocal.linear() * ray.getDirection();
    localDir.normalise();

    float bestT = std::numeric_limits<float>::max();
    int bestTriIndex = -1;
    int globalTriOffset = 0;

    for (size_t si = 0; si < m_editableMesh->subMeshes().size(); ++si) {
        const auto& sub = m_editableMesh->subMeshes()[si];
        for (size_t ti = 0; ti < sub.triangles.size(); ++ti) {
            const auto& tri = sub.triangles[ti];

            if (tri.indices[0] >= sub.vertices.size() ||
                tri.indices[1] >= sub.vertices.size() ||
                tri.indices[2] >= sub.vertices.size())
            {
                continue;
            }

            const Ogre::Vector3& v0 = sub.vertices[tri.indices[0]].position;
            const Ogre::Vector3& v1 = sub.vertices[tri.indices[1]].position;
            const Ogre::Vector3& v2 = sub.vertices[tri.indices[2]].position;

            float t = rayTriangleIntersect(localOrigin, localDir, v0, v1, v2);
            if (t >= 0.0f && t < bestT) {
                bestT = t;
                bestTriIndex = globalTriOffset + static_cast<int>(ti);
            }
        }
        globalTriOffset += static_cast<int>(sub.triangles.size());
    }

    return bestTriIndex;
}
// LCOV_EXCL_STOP

std::pair<int,int> EditModeController::hitTestEdge(const QPoint& screenPos,
                                                    Ogre::Camera* camera,
                                                    int viewportWidth, int viewportHeight,
                                                    float pixelRadius) const
{
    if (!m_editableMesh || !m_editEntity || !camera)
        return {-1, -1};

    Ogre::SceneNode* node = m_editEntity->getParentSceneNode();
    if (!node)
        return {-1, -1};

    float bestDist = pixelRadius;
    std::pair<int,int> bestEdge = {-1, -1};

    for (size_t si = 0; si < m_editableMesh->subMeshes().size(); ++si) {
        const auto& sub = m_editableMesh->subMeshes()[si];
        int vertOffset = localToGlobal(si, 0);

        for (const auto& tri : sub.triangles) {
            // Check each of the 3 edges in the triangle
            int localIndices[3] = {
                static_cast<int>(tri.indices[0]),
                static_cast<int>(tri.indices[1]),
                static_cast<int>(tri.indices[2])
            };

            for (int e = 0; e < 3; ++e) {
                int li0 = localIndices[e];
                int li1 = localIndices[(e + 1) % 3];

                if (li0 >= static_cast<int>(sub.vertices.size()) ||
                    li1 >= static_cast<int>(sub.vertices.size()))
                    continue;

                Ogre::Vector3 wp0 = node->convertLocalToWorldPosition(sub.vertices[li0].position);
                Ogre::Vector3 wp1 = node->convertLocalToWorldPosition(sub.vertices[li1].position);

                // Skip edges where both endpoints are behind the camera
                Ogre::Vector3 camPos = camera->getDerivedPosition();
                Ogre::Vector3 camDir = camera->getDerivedDirection();
                bool behind0 = (wp0 - camPos).dotProduct(camDir) < 0;
                bool behind1 = (wp1 - camPos).dotProduct(camDir) < 0;
                if (behind0 && behind1)
                    continue;

                QPoint sp0 = worldToScreen(wp0, camera, viewportWidth, viewportHeight);
                QPoint sp1 = worldToScreen(wp1, camera, viewportWidth, viewportHeight);

                float dist = pointToSegmentDistance(screenPos, sp0, sp1);
                if (dist < bestDist) {
                    bestDist = dist;
                    int g0 = vertOffset + li0;
                    int g1 = vertOffset + li1;
                    bestEdge = {std::min(g0, g1), std::max(g0, g1)};
                }
            }
        }
    }

    return bestEdge;
}

void EditModeController::boxSelectVertices(const QRect& rect,
                                            Ogre::Camera* camera,
                                            int viewportWidth, int viewportHeight,
                                            bool addToSelection)
{
    if (!m_editableMesh || !m_editEntity || !camera)
        return;

    Ogre::SceneNode* node = m_editEntity->getParentSceneNode();
    if (!node)
        return;

    if (!addToSelection)
        m_selectedVertices.clear();

    int globalOffset = 0;
    for (size_t si = 0; si < m_editableMesh->subMeshes().size(); ++si) {
        const auto& sub = m_editableMesh->subMeshes()[si];
        for (size_t vi = 0; vi < sub.vertices.size(); ++vi) {
            Ogre::Vector3 worldPos = node->convertLocalToWorldPosition(sub.vertices[vi].position);

            // Skip vertices behind camera
            Ogre::Vector3 camToVert = worldPos - camera->getDerivedPosition();
            if (camToVert.dotProduct(camera->getDerivedDirection()) < 0)
                continue;

            QPoint sp = worldToScreen(worldPos, camera, viewportWidth, viewportHeight);
            if (rect.contains(sp))
                m_selectedVertices.insert(globalOffset + static_cast<int>(vi));
        }
        globalOffset += static_cast<int>(sub.vertices.size());
    }

    updateSelectionOverlay();
    emit editSelectionChanged();
}

// ===========================================================================
// Mouse handling
// ===========================================================================

// LCOV_EXCL_START — mouse interaction requires active render window + camera
void EditModeController::handleMouseClick(const QPoint& screenPos, OgreWidget* widget,
                                           bool shiftHeld, bool ctrlHeld)
{
    if (!m_editModeActive || !widget)
        return;

    auto* cam = widget->getSpaceCamera();
    if (!cam || !cam->getCamera())
        return;

    auto* camera = cam->getCamera();
    auto* vp = camera->getViewport();
    if (!vp)
        return;

#ifdef Q_OS_MACOS
    int widthMod = 2;
#else
    int widthMod = 1;
#endif
    int vpWidth = vp->getActualWidth() / widthMod;
    int vpHeight = vp->getActualHeight() / widthMod;

    bool addToSelection = shiftHeld;
    bool removeFromSelection = ctrlHeld;

    switch (m_selectionMode) {
    case VertexMode: {
        int vi = hitTestVertex(screenPos, camera, vpWidth, vpHeight);
        if (vi >= 0) {
            if (removeFromSelection) {
                deselectVertex(vi);
            } else {
                selectVertex(vi, addToSelection);
            }
        } else if (!addToSelection && !removeFromSelection) {
            deselectAll();
        }
        break;
    }
    case EdgeMode: {
        auto edge = hitTestEdge(screenPos, camera, vpWidth, vpHeight);
        if (edge.first >= 0) {
            if (removeFromSelection) {
                deselectEdge(edge.first, edge.second);
            } else {
                selectEdge(edge.first, edge.second, addToSelection);
            }
        } else if (!addToSelection && !removeFromSelection) {
            deselectAll();
        }
        break;
    }
    case FaceMode: {
        int fi = hitTestFace(screenPos, camera, vpWidth, vpHeight);
        if (fi >= 0) {
            if (removeFromSelection) {
                deselectFace(fi);
            } else {
                selectFace(fi, addToSelection);
            }
        } else if (!addToSelection && !removeFromSelection) {
            deselectAll();
        }
        break;
    }
    }
}

void EditModeController::handleBoxSelect(const QPoint& startPos, const QPoint& endPos,
                                           OgreWidget* widget, bool shiftHeld)
{
    if (!m_editModeActive || !widget)
        return;

    auto* cam = widget->getSpaceCamera();
    if (!cam || !cam->getCamera())
        return;

    auto* camera = cam->getCamera();
    auto* vp = camera->getViewport();
    if (!vp)
        return;

#ifdef Q_OS_MACOS
    int widthMod = 2;
#else
    int widthMod = 1;
#endif
    int vpWidth = vp->getActualWidth() / widthMod;
    int vpHeight = vp->getActualHeight() / widthMod;

    QRect rect(startPos, endPos);
    rect = rect.normalized();

    boxSelectVertices(rect, camera, vpWidth, vpHeight, shiftHeld);
}
// LCOV_EXCL_STOP

// ===========================================================================
// Soft selection
// ===========================================================================

void EditModeController::setSoftSelectionEnabled(bool enabled)
{
    if (m_softSelectionEnabled != enabled) {
        m_softSelectionEnabled = enabled;
        SentryReporter::addBreadcrumb("edit_mode",
            QString("Soft selection %1").arg(enabled ? "enabled" : "disabled"));
        emit softSelectionChanged();
        updateSelectionOverlay();
    }
}

void EditModeController::setSoftSelectionRadius(double radius)
{
    if (radius > 0.0 && m_softSelectionRadius != radius) {
        m_softSelectionRadius = radius;
        emit softSelectionChanged();
        updateSelectionOverlay();
    }
}

void EditModeController::setSoftSelectionFalloff(int falloff)
{
    if (falloff >= 0 && falloff <= 1 && m_softSelectionFalloff != falloff) {
        m_softSelectionFalloff = falloff;
        emit softSelectionChanged();
        updateSelectionOverlay();
    }
}

std::map<int, float> EditModeController::getSoftSelectionWeights() const
{
    std::map<int, float> weights;
    if (!m_editableMesh || m_selectedVertices.empty())
        return weights;

    // All selected vertices get weight 1.0
    for (int gi : m_selectedVertices)
        weights[gi] = 1.0f;

    if (!m_softSelectionEnabled || m_softSelectionRadius <= 0.0)
        return weights;

    // Collect positions of selected vertices
    std::vector<Ogre::Vector3> selectedPositions;
    selectedPositions.reserve(m_selectedVertices.size());
    for (int gi : m_selectedVertices) {
        auto [subIdx, localIdx] = globalToLocal(gi);
        if (subIdx < m_editableMesh->subMeshes().size() &&
            localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size())
        {
            selectedPositions.push_back(
                m_editableMesh->subMeshes()[subIdx].vertices[localIdx].position);
        }
    }

    float radius = static_cast<float>(m_softSelectionRadius);
    int totalVerts = static_cast<int>(m_editableMesh->totalVertexCount());

    for (int gi = 0; gi < totalVerts; ++gi) {
        if (m_selectedVertices.count(gi))
            continue; // Already weight 1.0

        auto [subIdx, localIdx] = globalToLocal(gi);
        if (subIdx >= m_editableMesh->subMeshes().size())
            continue;
        const auto& pos = m_editableMesh->subMeshes()[subIdx].vertices[localIdx].position;

        // Find minimum distance to any selected vertex
        float minDist = std::numeric_limits<float>::max();
        for (const auto& selPos : selectedPositions) {
            float dist = pos.distance(selPos);
            if (dist < minDist)
                minDist = dist;
        }

        if (minDist < radius) {
            float t = minDist / radius; // 0 at selected vertex, 1 at radius boundary
            float weight = 0.0f;
            if (m_softSelectionFalloff == 0) {
                // Linear falloff
                weight = 1.0f - t;
            } else {
                // Smooth (cosine) falloff
                weight = 0.5f * (1.0f + std::cos(t * static_cast<float>(M_PI)));
            }
            if (weight > 0.001f)
                weights[gi] = weight;
        }
    }

    return weights;
}

// ===========================================================================
// Normals recalculation
// ===========================================================================

void EditModeController::setNormalsMode(int mode)
{
    if (mode >= 0 && mode <= 1 && m_normalsMode != mode) {
        m_normalsMode = mode;
        SentryReporter::addBreadcrumb("edit_mode",
            QString("Normals mode changed to %1").arg(mode == 0 ? "smooth" : "flat"));
        emit normalsModeChanged();
    }
}

void EditModeController::recalculateNormals(bool smooth)
{
    if (!m_editModeActive || !m_editableMesh || !m_editEntity)
        return;

    SentryReporter::addBreadcrumb("edit_mode",
        QString("Recalculate normals (%1)").arg(smooth ? "smooth" : "flat"));

    if (smooth)
        m_editableMesh->recalculateNormals();
    else
        m_editableMesh->recalculateNormalsFlat();

    // Write normals back to the GPU buffers for immediate visual feedback
    m_editableMesh->commitToEntity(m_editEntity);
    refreshNormalVisualizer();

    emit meshDataChanged();
}

// ===========================================================================
// Mesh validation after edits
// ===========================================================================

void EditModeController::validateMesh()
{
    if (!m_editableMesh) {
        m_degenerateTriangleCount = 0;
        emit validationChanged();
        return;
    }

    int oldCount = m_degenerateTriangleCount;
    m_degenerateTriangleCount = m_editableMesh->countDegenerateTriangles();
    if (m_degenerateTriangleCount != oldCount)
        emit validationChanged();
}

void EditModeController::removeDegenerateTriangles()
{
    if (!m_editModeActive || !m_editableMesh || !m_editEntity)
        return;

    SentryReporter::addBreadcrumb("edit_mode", "Remove degenerate triangles");

    int removed = m_editableMesh->removeDegenerateTriangles();

    if (removed > 0) {
        // recalculateNormals() already calls commitToEntity + refreshNormalVisualizer
        recalculateNormals(m_normalsMode == 0);
        updateSelectionOverlay();
    }

    // Re-validate
    validateMesh();
}

// ===========================================================================
// Vertex transform support
// ===========================================================================

Ogre::Vector3 EditModeController::getSelectedVerticesCentroid() const
{
    if (!m_editableMesh || m_selectedVertices.empty())
        return Ogre::Vector3::ZERO;

    Ogre::Vector3 centroid = Ogre::Vector3::ZERO;
    int count = 0;

    for (int gi : m_selectedVertices) {
        auto [subIdx, localIdx] = globalToLocal(gi);
        if (subIdx < m_editableMesh->subMeshes().size() &&
            localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size())
        {
            centroid += m_editableMesh->subMeshes()[subIdx].vertices[localIdx].position;
            ++count;
        }
    }

    if (count > 0)
        centroid /= static_cast<Ogre::Real>(count);

    return centroid;
}

void EditModeController::translateSelectedVertices(const Ogre::Vector3& delta)
{
    if (!m_editableMesh || m_selectedVertices.empty())
        return;

    auto weights = getSoftSelectionWeights();

    for (const auto& [gi, weight] : weights) {
        auto [subIdx, localIdx] = globalToLocal(gi);
        if (subIdx < m_editableMesh->subMeshes().size() &&
            localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size())
        {
            Ogre::Vector3 pos = m_editableMesh->getVertexPosition(subIdx, localIdx);
            pos += delta * weight;
            m_editableMesh->setVertexPosition(subIdx, localIdx, pos);
        }
    }

    // Recalculate normals and commit to GPU
    if (m_normalsMode == 0)
        m_editableMesh->recalculateNormals();
    else
        m_editableMesh->recalculateNormalsFlat();

    m_editableMesh->commitToEntity(m_editEntity);
    refreshNormalVisualizer();
    updateSelectionOverlay();
    emit meshDataChanged();
}

void EditModeController::rotateSelectedVertices(const Ogre::Quaternion& rotation)
{
    if (!m_editableMesh || m_selectedVertices.empty())
        return;

    Ogre::Vector3 pivot = getSelectedVerticesCentroid();
    auto weights = getSoftSelectionWeights();

    for (const auto& [gi, weight] : weights) {
        auto [subIdx, localIdx] = globalToLocal(gi);
        if (subIdx < m_editableMesh->subMeshes().size() &&
            localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size())
        {
            Ogre::Vector3 pos = m_editableMesh->getVertexPosition(subIdx, localIdx);
            Ogre::Vector3 offset = pos - pivot;
            // Interpolate rotation by weight
            Ogre::Quaternion weightedRot = Ogre::Quaternion::Slerp(weight, Ogre::Quaternion::IDENTITY, rotation);
            Ogre::Vector3 rotatedOffset = weightedRot * offset;
            m_editableMesh->setVertexPosition(subIdx, localIdx, pivot + rotatedOffset);
        }
    }

    if (m_normalsMode == 0)
        m_editableMesh->recalculateNormals();
    else
        m_editableMesh->recalculateNormalsFlat();

    m_editableMesh->commitToEntity(m_editEntity);
    refreshNormalVisualizer();
    updateSelectionOverlay();
    emit meshDataChanged();
}

void EditModeController::scaleSelectedVertices(const Ogre::Vector3& scaleFactor)
{
    if (!m_editableMesh || m_selectedVertices.empty())
        return;

    Ogre::Vector3 pivot = getSelectedVerticesCentroid();
    auto weights = getSoftSelectionWeights();

    for (const auto& [gi, weight] : weights) {
        auto [subIdx, localIdx] = globalToLocal(gi);
        if (subIdx < m_editableMesh->subMeshes().size() &&
            localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size())
        {
            Ogre::Vector3 pos = m_editableMesh->getVertexPosition(subIdx, localIdx);
            Ogre::Vector3 offset = pos - pivot;
            // Interpolate scale by weight
            Ogre::Vector3 weightedScale = Ogre::Vector3::UNIT_SCALE +
                (scaleFactor - Ogre::Vector3::UNIT_SCALE) * weight;
            Ogre::Vector3 scaledOffset = offset * weightedScale;
            m_editableMesh->setVertexPosition(subIdx, localIdx, pivot + scaledOffset);
        }
    }

    if (m_normalsMode == 0)
        m_editableMesh->recalculateNormals();
    else
        m_editableMesh->recalculateNormalsFlat();

    m_editableMesh->commitToEntity(m_editEntity);
    refreshNormalVisualizer();
    updateSelectionOverlay();
    emit meshDataChanged();
}

std::map<int, Ogre::Vector3> EditModeController::snapshotVertexPositions() const
{
    std::map<int, Ogre::Vector3> snapshot;
    if (!m_editableMesh)
        return snapshot;

    auto weights = getSoftSelectionWeights();
    for (const auto& [gi, weight] : weights) {
        auto [subIdx, localIdx] = globalToLocal(gi);
        if (subIdx < m_editableMesh->subMeshes().size() &&
            localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size())
        {
            snapshot[gi] = m_editableMesh->getVertexPosition(subIdx, localIdx);
        }
    }
    return snapshot;
}

void EditModeController::restoreVertexPositions(const std::map<int, Ogre::Vector3>& snapshot)
{
    if (!m_editableMesh)
        return;

    for (const auto& [gi, pos] : snapshot) {
        auto [subIdx, localIdx] = globalToLocal(gi);
        if (subIdx < m_editableMesh->subMeshes().size() &&
            localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size())
        {
            m_editableMesh->setVertexPosition(subIdx, localIdx, pos);
        }
    }

    if (m_editEntity) {
        m_editableMesh->commitToEntity(m_editEntity);
    refreshNormalVisualizer();
        updateSelectionOverlay();
        emit meshDataChanged();
    }
}

// ===========================================================================
// Selection overlay
// ===========================================================================

void EditModeController::createOverlayMaterials()
{
    auto& matMgr = Ogre::MaterialManager::getSingleton();

    // Vertex selection material (orange points, no lighting, no depth test)
    if (!matMgr.getByName("EditMode/VertexSelection"))
    {
        auto mat = matMgr.create("EditMode/VertexSelection",
                                 Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
        auto* pass = mat->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(false);
        pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
        pass->setDepthCheckEnabled(false);
        pass->setDepthWriteEnabled(false);
        pass->setPointSize(6.0f);
        pass->setPointSpritesEnabled(false);
    }

    // Edge selection material (colored lines, no lighting, no depth test)
    if (!matMgr.getByName("EditMode/EdgeSelection"))
    {
        auto mat = matMgr.create("EditMode/EdgeSelection",
                                 Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
        auto* pass = mat->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(false);
        pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
        pass->setDepthCheckEnabled(false);
        pass->setDepthWriteEnabled(false);
    }

    // Face selection material (semi-transparent overlay, no lighting)
    if (!matMgr.getByName("EditMode/FaceSelection"))
    {
        auto mat = matMgr.create("EditMode/FaceSelection",
                                 Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
        auto* pass = mat->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(false);
        pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
        pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        pass->setDepthCheckEnabled(true);
        pass->setDepthWriteEnabled(false);
        pass->setCullingMode(Ogre::CULL_NONE);
    }
}

void EditModeController::updateSelectionOverlay()
{
    if (!m_editModeActive || !m_editableMesh || !m_editEntity)
        return;

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    if (!sceneMgr)
        return;

    Ogre::SceneNode* parentNode = m_editEntity->getParentSceneNode();
    if (!parentNode)
        return;

    // Create the overlay scene node if needed (child of entity's node)
    if (!m_overlayNode)
    {
        m_overlayNode = parentNode->createChildSceneNode();
    }

    // -- Vertex overlay --
    if (!m_overlayVertices)
    {
        m_overlayVertices = sceneMgr->createManualObject("EditMode_VertexOverlay");
        m_overlayVertices->setDynamic(true);
        m_overlayVertices->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY);
        m_overlayNode->attachObject(m_overlayVertices);
    }

    m_overlayVertices->clear();
    if (!m_selectedVertices.empty())
    {
        m_overlayVertices->begin("EditMode/VertexSelection", Ogre::RenderOperation::OT_POINT_LIST);

        // Orange color for selected vertices
        Ogre::ColourValue vertColor(1.0f, 0.6f, 0.0f, 1.0f);

        for (int gi : m_selectedVertices) {
            auto [subIdx, localIdx] = globalToLocal(gi);
            if (subIdx < m_editableMesh->subMeshes().size() &&
                localIdx < m_editableMesh->subMeshes()[subIdx].vertices.size())
            {
                const auto& pos = m_editableMesh->subMeshes()[subIdx].vertices[localIdx].position;
                m_overlayVertices->position(pos);
                m_overlayVertices->colour(vertColor);
            }
        }

        m_overlayVertices->end();
    }

    // -- Edge overlay --
    if (!m_overlayEdges)
    {
        m_overlayEdges = sceneMgr->createManualObject("EditMode_EdgeOverlay");
        m_overlayEdges->setDynamic(true);
        m_overlayEdges->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY);
        m_overlayNode->attachObject(m_overlayEdges);
    }

    m_overlayEdges->clear();
    if (!m_selectedEdges.empty())
    {
        m_overlayEdges->begin("EditMode/EdgeSelection", Ogre::RenderOperation::OT_LINE_LIST);

        // Cyan color for selected edges
        Ogre::ColourValue edgeColor(0.0f, 1.0f, 1.0f, 1.0f);

        for (const auto& [g0, g1] : m_selectedEdges) {
            auto [sub0, loc0] = globalToLocal(g0);
            auto [sub1, loc1] = globalToLocal(g1);

            if (sub0 < m_editableMesh->subMeshes().size() &&
                loc0 < m_editableMesh->subMeshes()[sub0].vertices.size() &&
                sub1 < m_editableMesh->subMeshes().size() &&
                loc1 < m_editableMesh->subMeshes()[sub1].vertices.size())
            {
                const auto& p0 = m_editableMesh->subMeshes()[sub0].vertices[loc0].position;
                const auto& p1 = m_editableMesh->subMeshes()[sub1].vertices[loc1].position;
                m_overlayEdges->position(p0);
                m_overlayEdges->colour(edgeColor);
                m_overlayEdges->position(p1);
                m_overlayEdges->colour(edgeColor);
            }
        }

        m_overlayEdges->end();
    }

    // -- Face overlay --
    if (!m_overlayFaces)
    {
        m_overlayFaces = sceneMgr->createManualObject("EditMode_FaceOverlay");
        m_overlayFaces->setDynamic(true);
        m_overlayFaces->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY);
        m_overlayNode->attachObject(m_overlayFaces);
    }

    m_overlayFaces->clear();
    if (!m_selectedFaces.empty())
    {
        m_overlayFaces->begin("EditMode/FaceSelection", Ogre::RenderOperation::OT_TRIANGLE_LIST);

        // Semi-transparent blue tint for selected faces
        Ogre::ColourValue faceColor(0.2f, 0.4f, 1.0f, 0.35f);

        for (int gi : m_selectedFaces) {
            auto [subIdx, localTri] = globalTriToLocal(gi);
            if (subIdx < m_editableMesh->subMeshes().size() &&
                localTri < m_editableMesh->subMeshes()[subIdx].triangles.size())
            {
                const auto& tri = m_editableMesh->subMeshes()[subIdx].triangles[localTri];
                const auto& verts = m_editableMesh->subMeshes()[subIdx].vertices;

                if (tri.indices[0] < verts.size() &&
                    tri.indices[1] < verts.size() &&
                    tri.indices[2] < verts.size())
                {
                    // Offset slightly toward camera to avoid z-fighting
                    Ogre::Vector3 faceNormal =
                        (verts[tri.indices[1]].position - verts[tri.indices[0]].position)
                            .crossProduct(verts[tri.indices[2]].position - verts[tri.indices[0]].position);
                    faceNormal.normalise();
                    Ogre::Vector3 offset = faceNormal * 0.001f;

                    m_overlayFaces->position(verts[tri.indices[0]].position + offset);
                    m_overlayFaces->colour(faceColor);
                    m_overlayFaces->position(verts[tri.indices[1]].position + offset);
                    m_overlayFaces->colour(faceColor);
                    m_overlayFaces->position(verts[tri.indices[2]].position + offset);
                    m_overlayFaces->colour(faceColor);
                }
            }
        }

        m_overlayFaces->end();
    }

    // -- Soft selection radius sphere --
    if (m_softSelectionEnabled && !m_selectedVertices.empty()) {
        Ogre::Vector3 centroid = getSelectedVerticesCentroid();

        // Create sphere entity on first use
        if (!m_softSelSphere) {
            const std::string meshName = "__SoftSelSphere__";
            if (!Ogre::MeshManager::getSingleton().resourceExists(meshName)) {
                Procedural::SphereGenerator()
                    .setRadius(1.0f)
                    .setNumRings(16)
                    .setNumSegments(16)
                    .realizeMesh(meshName);
            }

            // Create a transparent glass-like material
            const std::string matName = "EditMode/SoftSelRadius";
            auto& matMgr = Ogre::MaterialManager::getSingleton();
            if (!matMgr.resourceExists(matName)) {
                auto mat = matMgr.create(matName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
                auto* pass = mat->getTechnique(0)->getPass(0);
                pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
                pass->setDepthWriteEnabled(false);
                pass->setDiffuse(Ogre::ColourValue(0.3f, 0.6f, 1.0f, 0.08f));
                pass->setAmbient(Ogre::ColourValue(0.2f, 0.4f, 0.8f, 0.08f));
                pass->setSpecular(Ogre::ColourValue(1.0f, 1.0f, 1.0f, 0.15f));
                pass->setShininess(60.0f);
                pass->setCullingMode(Ogre::CULL_NONE);
            }

            m_softSelSphereNode = m_overlayNode->createChildSceneNode();
            m_softSelSphere = sceneMgr->createEntity("EditMode_SoftSelSphere", meshName);
            m_softSelSphere->setMaterialName(matName);
            m_softSelSphere->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY - 1);
            m_softSelSphereNode->attachObject(m_softSelSphere);
        }

        m_softSelSphereNode->setPosition(centroid);
        float r = static_cast<float>(m_softSelectionRadius);
        m_softSelSphereNode->setScale(r, r, r);
        m_softSelSphereNode->setVisible(true);
    } else if (m_softSelSphereNode) {
        m_softSelSphereNode->setVisible(false);
    }
}

void EditModeController::destroySelectionOverlay()
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr)
        return;
    auto* sceneMgr = mgr->getSceneMgr();
    if (!sceneMgr)
        return;

    if (m_overlayNode)
    {
        m_overlayNode->detachAllObjects();

        if (m_overlayVertices) {
            sceneMgr->destroyManualObject(m_overlayVertices);
            m_overlayVertices = nullptr;
        }
        if (m_overlayEdges) {
            sceneMgr->destroyManualObject(m_overlayEdges);
            m_overlayEdges = nullptr;
        }
        if (m_overlayFaces) {
            sceneMgr->destroyManualObject(m_overlayFaces);
            m_overlayFaces = nullptr;
        }
        if (m_softSelSphere) {
            if (m_softSelSphereNode)
                m_softSelSphereNode->detachAllObjects();
            sceneMgr->destroyEntity(m_softSelSphere);
            m_softSelSphere = nullptr;
            if (m_softSelSphereNode) {
                sceneMgr->destroySceneNode(m_softSelSphereNode);
                m_softSelSphereNode = nullptr;
            }
        }

        sceneMgr->destroySceneNode(m_overlayNode);
        m_overlayNode = nullptr;
    }
}


// ===========================================================================
// Wireframe toggle
// ===========================================================================

void EditModeController::applyWireframeMaterials()
{
    if (!m_editEntity)
        return;

    m_savedMaterials.clear();
    for (unsigned int i = 0; i < m_editEntity->getNumSubEntities(); ++i) {
        auto* subEnt = m_editEntity->getSubEntity(i);
        m_savedMaterials[i] = subEnt->getMaterialName();

        Ogre::String wireName = subEnt->getMaterialName() + "_EditWireframe";
        auto& matMgr = Ogre::MaterialManager::getSingleton();
        auto wireMat = matMgr.getByName(wireName);
        if (!wireMat)
            wireMat = subEnt->getMaterial()->clone(wireName);

        for (unsigned short ti = 0; ti < wireMat->getNumTechniques(); ++ti) {
            for (unsigned short pi = 0; pi < wireMat->getTechnique(ti)->getNumPasses(); ++pi) {
                wireMat->getTechnique(ti)->getPass(pi)->setPolygonMode(Ogre::PM_WIREFRAME);
            }
        }
        subEnt->setMaterialName(wireName);
    }
}

void EditModeController::removeWireframeMaterials()
{
    if (!m_editEntity)
        return;

    for (const auto& [idx, matName] : m_savedMaterials) {
        if (idx < m_editEntity->getNumSubEntities()) {
            m_editEntity->getSubEntity(idx)->setMaterialName(matName);
            Ogre::String wireName = matName + "_EditWireframe";
            auto& matMgr = Ogre::MaterialManager::getSingleton();
            if (matMgr.resourceExists(wireName))
                matMgr.remove(wireName);
        }
    }
    m_savedMaterials.clear();
}

void EditModeController::setWireframeEnabled(bool enabled)
{
    if (m_wireframeEnabled == enabled)
        return;

    m_wireframeEnabled = enabled;

    if (m_editModeActive && m_editEntity) {
        if (enabled)
            applyWireframeMaterials();
        else
            removeWireframeMaterials();
    }

    SentryReporter::addBreadcrumb("edit_mode",
        QString("Wireframe %1").arg(enabled ? "enabled" : "disabled"));
    emit wireframeChanged();
}

void EditModeController::refreshNormalVisualizer()
{
    if (!m_editEntity) return;
    auto* mainWin = Manager::getSingletonPtr() ? Manager::getSingleton()->getMainWindow() : nullptr;
    if (!mainWin) return;
    auto* nv = mainWin->findChild<NormalVisualizer*>();
    if (nv) nv->refreshEntity(m_editEntity);
}
