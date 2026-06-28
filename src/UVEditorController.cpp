#include "UVEditorController.h"

#include "EditableMesh.h"
#include "EditModeController.h"
#include "HalfEdgeMesh.h"
#include "EmbeddedTextureCache.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "ThemeManager.h"
#include "UndoManager.h"
#include "UvSeamData.h"
#include "UvSeamOps.h"
#include "UvUnwrap.h"
#include "commands/UVEditCommand.h"
#include "commands/UvSeamCommands.h"

#include <QImage>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QLibraryInfo>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <OgreEntity.h>
#include <OgreMaterial.h>
#include <OgrePass.h>
#include <OgreSubEntity.h>
#include <OgreSubMesh.h>
#include <OgreTexture.h>
#include <OgreTextureManager.h>
#include <OgreTextureUnitState.h>

#include <queue>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <utility>

UVEditorController* UVEditorController::s_instance = nullptr;

UVEditorController* UVEditorController::instance()
{
    if (!s_instance)
        s_instance = new UVEditorController();
    return s_instance;
}

UVEditorController* UVEditorController::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    Q_UNUSED(engine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void UVEditorController::kill()
{
    delete s_instance;
    s_instance = nullptr;
}

UVEditorController::UVEditorController(QObject* parent)
    : QObject(parent)
{
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(75);
    connect(m_refreshTimer, &QTimer::timeout, this, &UVEditorController::rebuildMeshCache);

    connectSignals();
}

void UVEditorController::setPanelActive(bool active)
{
    if (m_panelActive == active)
        return;
    m_panelActive = active;
    if (!m_panelActive) {
        if (m_refreshTimer->isActive()) {
            m_refreshTimer->stop();
            m_refreshPending = true;
        }
        return;
    }
    if (m_refreshPending) {
        m_refreshPending = false;
        refresh();
    }
}

void UVEditorController::updateSurfacesActive()
{
    setPanelActive(m_inspectorEmbedded || m_editorWindow != nullptr);
}

void UVEditorController::setInspectorEmbedded(bool embedded)
{
    if (m_inspectorEmbedded == embedded)
        return;
    m_inspectorEmbedded = embedded;
    updateSurfacesActive();
    if (m_inspectorEmbedded)
        refresh();
}

void UVEditorController::openEditorWindow()
{
    if (m_editorWindow) {
        if (auto* w = qobject_cast<QQuickWindow*>(m_editorWindow)) {
            w->show();
            w->raise();
            w->requestActivate();
        }
        return;
    }

    auto* engine = new QQmlApplicationEngine(this);
    const QString appDir = QCoreApplication::applicationDirPath();
    engine->addImportPath(appDir + QStringLiteral("/qml"));
    engine->addImportPath(QLibraryInfo::path(QLibraryInfo::QmlImportsPath));

    qmlRegisterSingletonType<ThemeManager>(
        "ThemeManager", 1, 0, "ThemeManager",
        [](QQmlEngine* e, QJSEngine*) -> QObject* {
            return ThemeManager::qmlInstance(e, nullptr);
        });
    qmlRegisterSingletonType<UVEditorController>(
        "PropertiesPanel", 1, 0, "UVEditorController",
        [](QQmlEngine* e, QJSEngine*) -> QObject* {
            return UVEditorController::qmlInstance(e, nullptr);
        });

    bool handled = false;
    connect(engine, &QQmlApplicationEngine::objectCreated, this,
            [this, engine, &handled](QObject* obj, const QUrl&) {
                handled = true;
                if (!obj) {
                    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                                  QStringLiteral("UV editor window: QML load failed"));
                    engine->deleteLater();
                    return;
                }
                m_editorWindow = obj;
                updateSurfacesActive();
                refresh();
                emit editorWindowChanged();
                if (auto* w = qobject_cast<QQuickWindow*>(obj)) {
                    connect(w, &QQuickWindow::visibleChanged, this,
                            [this, w, engine](bool vis) {
                                if (vis || m_editorWindow != w)
                                    return;
                                m_editorWindow = nullptr;
                                updateSurfacesActive();
                                emit editorWindowChanged();
                                engine->deleteLater();
                            });
                    w->show();
                    w->raise();
                    w->requestActivate();
                }
            },
            Qt::DirectConnection);

    engine->load(QUrl(QStringLiteral("qrc:/UVEditor/UVEditorWindow.qml")));
    if (!handled) {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("UV editor window: load() returned without objectCreated"));
    }
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("UV editor window opened"));
}

void UVEditorController::closeEditorWindow()
{
    if (!m_editorWindow)
        return;
    if (auto* w = qobject_cast<QQuickWindow*>(m_editorWindow))
        w->close();
    else
        m_editorWindow = nullptr;
}

void UVEditorController::scheduleRefresh()
{
    if (!m_panelActive) {
        m_refreshPending = true;
        return;
    }
    m_refreshTimer->start();
}

void UVEditorController::connectSignals()
{
    if (auto* sel = SelectionSet::getSingleton()) {
        connect(sel, &SelectionSet::selectionChanged, this, &UVEditorController::scheduleRefresh);
    }
    if (auto* mgr = Manager::getSingletonPtr()) {
        connect(mgr, &Manager::entityCreated, this, &UVEditorController::scheduleRefresh);
        connect(mgr, &Manager::sceneNodeDestroyed, this, &UVEditorController::scheduleRefresh);
    }
    if (auto* edit = EditModeController::instance()) {
        connect(edit, &EditModeController::meshDataChanged, this, &UVEditorController::scheduleRefresh);
        connect(edit, &EditModeController::editModeChanged, this, &UVEditorController::scheduleRefresh);
        connect(edit, &EditModeController::editSelectionChanged, this,
                &UVEditorController::onEditSelectionChanged);
    }
}

void UVEditorController::setUvChannel(int channel)
{
    channel = std::max(0, std::min(channel, 1));
    if (m_uvChannel == channel)
        return;
    m_uvChannel = channel;
    emit uvChannelChanged();
    rebuildMeshCache();
}

void UVEditorController::setSelectionMode(int mode)
{
    mode = std::max(0, std::min(mode, 2));
    const auto next = static_cast<SelectionMode>(mode);
    if (m_selectionMode == next)
        return;
    m_selectionMode = next;
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("UV editor selection mode: %1").arg(mode));
    emit selectionModeChanged();
}

void UVEditorController::clearUvSelection()
{
    if (m_selectedUvVerts.isEmpty() && m_selectedUvEdges.isEmpty() && m_selectedUvFaces.isEmpty())
        return;
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("UV editor clear selection"));
    m_selectedUvVerts.clear();
    m_selectedUvEdges.clear();
    m_selectedUvFaces.clear();
    notifyUvSelectionChanged();
}

void UVEditorController::notifyUvSelectionChanged()
{
    ++m_selectionRevision;
    emit uvSelectionChanged();
}

namespace {

constexpr float kUvEpsilon = 1e-5f;

bool pointInTriangle(double u, double v, float u0, float v0, float u1, float v1, float u2, float v2)
{
    const double dX = static_cast<double>(u) - static_cast<double>(u2);
    const double dY = static_cast<double>(v) - static_cast<double>(v2);
    const double dX21 = static_cast<double>(u2) - static_cast<double>(u1);
    const double dY12 = static_cast<double>(v1) - static_cast<double>(v2);
    const double D = dY12 * (static_cast<double>(u0) - static_cast<double>(u2))
                     + dX21 * (static_cast<double>(v0) - static_cast<double>(v2));
    if (std::abs(D) < 1e-12)
        return false;
    const double s = dY12 * dX + dX21 * dY;
    const double t = (static_cast<double>(v2) - static_cast<double>(v0)) * dX
                     + (static_cast<double>(u0) - static_cast<double>(u2)) * dY;
    if (D < 0)
        return s <= 0 && t <= 0 && s + t >= D;
    return s >= 0 && t >= 0 && s + t <= D;
}

double distPointToSegmentSq(double u, double v, float u0, float v0, float u1, float v1)
{
    const double dx = static_cast<double>(u1) - static_cast<double>(u0);
    const double dy = static_cast<double>(v1) - static_cast<double>(v0);
    const double lenSq = dx * dx + dy * dy;
    if (lenSq < 1e-18)
        return (u - u0) * (u - u0) + (v - v0) * (v - v0);
    double t = ((u - u0) * dx + (v - v0) * dy) / lenSq;
    t = std::max(0.0, std::min(1.0, t));
    const double px = u0 + t * dx;
    const double py = v0 + t * dy;
    const double ddx = u - px;
    const double ddy = v - py;
    return ddx * ddx + ddy * ddy;
}

bool segmentIntersectsRect(double u0, double v0, double u1, double v1,
                           double minU, double minV, double maxU, double maxV)
{
    if ((u0 >= minU && u0 <= maxU && v0 >= minV && v0 <= maxV)
        || (u1 >= minU && u1 <= maxU && v1 >= minV && v1 <= maxV))
        return true;
    const auto inside = [&](double u, double v) {
        return u >= minU && u <= maxU && v >= minV && v <= maxV;
    };
    const double rectU[4] = {minU, maxU, maxU, minU};
    const double rectV[4] = {minV, minV, maxV, maxV};
    for (int i = 0; i < 4; ++i) {
        const double ru0 = rectU[i];
        const double rv0 = rectV[i];
        const double ru1 = rectU[(i + 1) % 4];
        const double rv1 = rectV[(i + 1) % 4];
        const double d1x = u1 - u0;
        const double d1y = v1 - v0;
        const double d2x = ru1 - ru0;
        const double d2y = rv1 - rv0;
        const double denom = d1x * d2y - d1y * d2x;
        if (std::abs(denom) < 1e-18)
            continue;
        const double t = ((ru0 - u0) * d2y - (rv0 - v0) * d2x) / denom;
        const double s = ((ru0 - u0) * d1y - (rv0 - v0) * d1x) / denom;
        if (t >= 0.0 && t <= 1.0 && s >= 0.0 && s <= 1.0)
            return true;
    }
    return inside(u0, v0) || inside(u1, v1);
}

bool triangleTouchesRect(const float u[3], const float v[3],
                         double minU, double minV, double maxU, double maxV)
{
    for (int c = 0; c < 3; ++c) {
        if (u[c] >= minU && u[c] <= maxU && v[c] >= minV && v[c] <= maxV)
            return true;
    }
    const double cx = (u[0] + u[1] + u[2]) / 3.0;
    const double cy = (v[0] + v[1] + v[2]) / 3.0;
    if (cx >= minU && cx <= maxU && cy >= minV && cy <= maxV)
        return true;
    for (int e = 0; e < 3; ++e) {
        const int n = (e + 1) % 3;
        if (segmentIntersectsRect(u[e], v[e], u[n], v[n], minU, minV, maxU, maxV))
            return true;
    }
    return pointInTriangle((minU + maxU) * 0.5, (minV + maxV) * 0.5,
                           u[0], v[0], u[1], v[1], u[2], v[2]);
}

class UnionFind {
public:
    explicit UnionFind(int n) : parent(n), rank(n, 0)
    {
        for (int i = 0; i < n; ++i)
            parent[i] = i;
    }

    int find(int x)
    {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }

    void unite(int a, int b)
    {
        a = find(a);
        b = find(b);
        if (a == b)
            return;
        if (rank[a] < rank[b])
            std::swap(a, b);
        parent[b] = a;
        if (rank[a] == rank[b])
            ++rank[a];
    }

private:
    std::vector<int> parent;
    std::vector<int> rank;
};

} // namespace

void UVEditorController::applySelectionSet(const QSet<int>& verts, const QSet<int>& edges,
                                           const QSet<int>& faces, int modifiers)
{
    const bool add = (modifiers & static_cast<int>(ShiftModifier)) != 0;
    const bool toggle = (modifiers & static_cast<int>(ControlModifier)) != 0;

    if (!add && !toggle) {
        m_selectedUvVerts = verts;
        m_selectedUvEdges = edges;
        m_selectedUvFaces = faces;
    } else if (add) {
        m_selectedUvVerts.unite(verts);
        m_selectedUvEdges.unite(edges);
        m_selectedUvFaces.unite(faces);
    } else {
        for (int id : verts) {
            if (m_selectedUvVerts.contains(id))
                m_selectedUvVerts.remove(id);
            else
                m_selectedUvVerts.insert(id);
        }
        for (int id : edges) {
            if (m_selectedUvEdges.contains(id))
                m_selectedUvEdges.remove(id);
            else
                m_selectedUvEdges.insert(id);
        }
        for (int id : faces) {
            if (m_selectedUvFaces.contains(id))
                m_selectedUvFaces.remove(id);
            else
                m_selectedUvFaces.insert(id);
        }
    }

    notifyUvSelectionChanged();
}

void UVEditorController::pickAt(double u, double v, int modifiers, double pickRadiusUv)
{
    if (!m_hasMesh || m_uvTris.empty())
        return;

    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("UV editor pick selection"));

    const double pickRadiusSq = pickRadiusUv * pickRadiusUv;
    QSet<int> verts;
    QSet<int> edges;
    QSet<int> faces;

    if (m_selectionMode == FaceMode) {
        int bestFace = -1;
        double bestDistSq = 1e30;
        bool insideTri = false;
        for (size_t fi = 0; fi < m_uvTris.size(); ++fi) {
            const auto& tri = m_uvTris[fi];
            if (pointInTriangle(u, v, tri.u[0], tri.v[0], tri.u[1], tri.v[1], tri.u[2], tri.v[2])) {
                bestFace = static_cast<int>(fi);
                insideTri = true;
                break;
            }
            const double cx = (tri.u[0] + tri.u[1] + tri.u[2]) / 3.0;
            const double cy = (tri.v[0] + tri.v[1] + tri.v[2]) / 3.0;
            const double d = (u - cx) * (u - cx) + (v - cy) * (v - cy);
            if (d < bestDistSq) {
                bestDistSq = d;
                bestFace = static_cast<int>(fi);
            }
        }
        if (insideTri || (bestFace >= 0 && bestDistSq <= pickRadiusSq))
            faces.insert(bestFace);
    } else if (m_selectionMode == EdgeMode) {
        int bestEdge = -1;
        double bestDistSq = 1e30;
        for (size_t ei = 0; ei < m_uvEdges.size(); ++ei) {
            const auto& edge = m_uvEdges[ei];
            if (edge.v0 < 0 || edge.v1 < 0
                || edge.v0 >= static_cast<int>(m_uvVerts.size())
                || edge.v1 >= static_cast<int>(m_uvVerts.size()))
                continue;
            const auto& a = m_uvVerts[edge.v0];
            const auto& b = m_uvVerts[edge.v1];
            const double d = distPointToSegmentSq(u, v, a.u, a.v, b.u, b.v);
            if (d < bestDistSq) {
                bestDistSq = d;
                bestEdge = static_cast<int>(ei);
            }
        }
        if (bestEdge >= 0 && bestDistSq <= pickRadiusSq)
            edges.insert(bestEdge);
    } else {
        int bestVert = -1;
        double bestDistSq = 1e30;
        for (size_t vi = 0; vi < m_uvVerts.size(); ++vi) {
            const auto& vert = m_uvVerts[vi];
            const double d = (u - vert.u) * (u - vert.u) + (v - vert.v) * (v - vert.v);
            if (d < bestDistSq) {
                bestDistSq = d;
                bestVert = static_cast<int>(vi);
            }
        }
        if (bestVert >= 0 && bestDistSq <= pickRadiusSq)
            verts.insert(bestVert);
    }

    applySelectionSet(verts, edges, faces, modifiers);
}

void UVEditorController::boxSelect(double uMin, double vMin, double uMax, double vMax, int modifiers)
{
    if (!m_hasMesh)
        return;

    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("UV editor box selection"));

    if (uMin > uMax)
        std::swap(uMin, uMax);
    if (vMin > vMax)
        std::swap(vMin, vMax);

    QSet<int> verts;
    QSet<int> edges;
    QSet<int> faces;

    if (m_selectionMode == VertexMode) {
        for (size_t vi = 0; vi < m_uvVerts.size(); ++vi) {
            const auto& vert = m_uvVerts[vi];
            if (vert.u >= uMin && vert.u <= uMax && vert.v >= vMin && vert.v <= vMax)
                verts.insert(static_cast<int>(vi));
        }
    } else if (m_selectionMode == EdgeMode) {
        for (size_t ei = 0; ei < m_uvEdges.size(); ++ei) {
            const auto& edge = m_uvEdges[ei];
            if (edge.v0 < 0 || edge.v1 < 0
                || edge.v0 >= static_cast<int>(m_uvVerts.size())
                || edge.v1 >= static_cast<int>(m_uvVerts.size()))
                continue;
            const auto& a = m_uvVerts[edge.v0];
            const auto& b = m_uvVerts[edge.v1];
            if (segmentIntersectsRect(a.u, a.v, b.u, b.v, uMin, vMin, uMax, vMax))
                edges.insert(static_cast<int>(ei));
        }
    } else {
        for (size_t fi = 0; fi < m_uvTris.size(); ++fi) {
            if (triangleTouchesRect(m_uvTris[fi].u, m_uvTris[fi].v, uMin, vMin, uMax, vMax))
                faces.insert(static_cast<int>(fi));
        }
    }

    applySelectionSet(verts, edges, faces, modifiers);
}

QVariantList UVEditorController::selectionVertices() const
{
    QVariantList out;
    for (int id : m_selectedUvVerts) {
        if (id < 0 || id >= static_cast<int>(m_uvVerts.size()))
            continue;
        const auto& v = m_uvVerts[id];
        out.push_back(QVariantMap{
            {QStringLiteral("u"), v.u},
            {QStringLiteral("v"), v.v}
        });
    }
    return out;
}

QVariantList UVEditorController::selectionEdges() const
{
    QVariantList out;
    for (int id : m_selectedUvEdges) {
        if (id < 0 || id >= static_cast<int>(m_uvEdges.size()))
            continue;
        const auto& e = m_uvEdges[id];
        if (e.v0 < 0 || e.v1 < 0
            || e.v0 >= static_cast<int>(m_uvVerts.size())
            || e.v1 >= static_cast<int>(m_uvVerts.size()))
            continue;
        const auto& a = m_uvVerts[e.v0];
        const auto& b = m_uvVerts[e.v1];
        out.push_back(QVariantMap{
            {QStringLiteral("u0"), a.u},
            {QStringLiteral("v0"), a.v},
            {QStringLiteral("u1"), b.u},
            {QStringLiteral("v1"), b.v}
        });
    }
    return out;
}

QVariantList UVEditorController::selectionFaces() const
{
    QVariantList out;
    for (int id : m_selectedUvFaces) {
        if (id < 0 || id >= static_cast<int>(m_uvTris.size()))
            continue;
        const auto& tri = m_uvTris[id];
        out.push_back(QVariantMap{
            {QStringLiteral("u0"), tri.u[0]},
            {QStringLiteral("v0"), tri.v[0]},
            {QStringLiteral("u1"), tri.u[1]},
            {QStringLiteral("v1"), tri.v[1]},
            {QStringLiteral("u2"), tri.u[2]},
            {QStringLiteral("v2"), tri.v[2]}
        });
    }
    return out;
}

QVariantList UVEditorController::contextIslandFaces() const
{
    QVariantList out;
    for (size_t fi = 0; fi < m_uvTris.size(); ++fi) {
        if (!m_contextIslandIds.contains(m_uvTris[fi].island))
            continue;
        const auto& tri = m_uvTris[fi];
        out.push_back(QVariantMap{
            {QStringLiteral("u0"), tri.u[0]},
            {QStringLiteral("v0"), tri.v[0]},
            {QStringLiteral("u1"), tri.u[1]},
            {QStringLiteral("v1"), tri.v[1]},
            {QStringLiteral("u2"), tri.u[2]},
            {QStringLiteral("v2"), tri.v[2]}
        });
    }
    return out;
}

void UVEditorController::onEditSelectionChanged()
{
    if (!m_panelActive) {
        m_refreshPending = true;
        return;
    }
    updateContextIslandsFromEdit();
    notifyUvSelectionChanged();
}

void UVEditorController::updateContextIslandsFromEdit()
{
    m_contextIslandIds.clear();
    auto* edit = EditModeController::instance();
    if (!edit || !edit->isEditModeActive() || !m_activeEntity
        || edit->editEntity() != m_activeEntity)
        return;

    QSet<int> islands;
    for (int gv : edit->selectedVertices()) {
        if (gv < 0 || gv >= static_cast<int>(m_trisByGlobalVert.size()))
            continue;
        for (int fi : m_trisByGlobalVert[static_cast<size_t>(gv)])
            islands.insert(m_uvTris[static_cast<size_t>(fi)].island);
    }
    for (const auto& edge : edit->selectedEdges()) {
        const int a = edge.first;
        const int b = edge.second;
        if (a < 0 || a >= static_cast<int>(m_trisByGlobalVert.size()))
            continue;
        for (int fi : m_trisByGlobalVert[static_cast<size_t>(a)]) {
            const auto& tri = m_uvTris[static_cast<size_t>(fi)];
            for (int e = 0; e < 3; ++e) {
                const int n = (e + 1) % 3;
                const int va = tri.meshGlobalVert[e];
                const int vb = tri.meshGlobalVert[n];
                if ((va == a && vb == b) || (va == b && vb == a)) {
                    islands.insert(tri.island);
                    break;
                }
            }
        }
    }
    for (int gt : edit->selectedFaces()) {
        const auto it = m_fiByGlobalTri.find(gt);
        if (it != m_fiByGlobalTri.end())
            islands.insert(m_uvTris[static_cast<size_t>(it->second)].island);
    }

    m_contextIslandIds = islands;
}

void UVEditorController::setShowTextureBackground(bool on)
{
    if (m_showTextureBackground == on)
        return;
    m_showTextureBackground = on;
    updateTexturePixelSize();
    emit showTextureBackgroundChanged();
}

void UVEditorController::setTransformMode(int mode)
{
    mode = std::max(-1, std::min(mode, 2));
    const auto next = static_cast<TransformMode>(mode);
    if (m_transformMode == next)
        return;
    m_transformMode = next;
    emit transformModeChanged();
}

void UVEditorController::setPivotMode(int mode)
{
    mode = std::max(0, std::min(mode, 2));
    const auto next = static_cast<PivotMode>(mode);
    if (m_pivotMode == next)
        return;
    m_pivotMode = next;
    emit pivotModeChanged();
}

void UVEditorController::setSnapMode(int mode)
{
    mode = std::max(0, std::min(mode, 2));
    const auto next = static_cast<SnapMode>(mode);
    if (m_snapMode == next)
        return;
    m_snapMode = next;
    emit snapModeChanged();
}

void UVEditorController::setSnapEnabled(bool on)
{
    if (m_snapEnabled == on)
        return;
    m_snapEnabled = on;
    emit snapEnabledChanged();
}

void UVEditorController::setUseBlenderTransformKeys(bool on)
{
    if (m_useBlenderTransformKeys == on)
        return;
    m_useBlenderTransformKeys = on;
    emit useBlenderTransformKeysChanged();
}

void UVEditorController::setCursorU(double u)
{
    if (std::abs(m_cursorU - u) < 1e-9)
        return;
    m_cursorU = u;
    emit cursorChanged();
}

void UVEditorController::setCursorV(double v)
{
    if (std::abs(m_cursorV - v) < 1e-9)
        return;
    m_cursorV = v;
    emit cursorChanged();
}

void UVEditorController::setCursorFromUv(double u, double v)
{
    setCursorU(u);
    setCursorV(v);
}

EditableMesh* UVEditorController::workingMeshForEntity(Ogre::Entity* entity)
{
    if (!entity || entity != m_activeEntity || m_workingMesh.subMeshes().empty())
        return nullptr;
    return &m_workingMesh;
}

void UVEditorController::refreshAfterUvEdit()
{
    refresh();
}

void UVEditorController::syncWorkingMeshFromEditable(const EditableMesh& mesh)
{
    if (!m_activeEntity)
        return;
    m_workingMesh.subMeshes() = mesh.subMeshes();
    refreshAfterUvEdit();
}

void UVEditorController::syncWorkingMeshFromEntity()
{
    if (!m_activeEntity)
        return;

    if (auto* edit = EditModeController::instance()) {
        if (edit->isEditModeActive() && edit->editEntity() == m_activeEntity && edit->currentMesh()) {
            m_workingMesh.subMeshes() = edit->currentMesh()->subMeshes();
            applyUvChannel(m_workingMesh, m_activeEntity, m_uvChannel, m_submeshFilter);
            return;
        }
    }

    if (!m_workingMesh.loadFromEntity(m_activeEntity))
        return;
    applyUvChannel(m_workingMesh, m_activeEntity, m_uvChannel, m_submeshFilter);

    if (auto* edit = EditModeController::instance()) {
        if (edit->isEditModeActive() && edit->editEntity() == m_activeEntity && edit->currentMesh())
            edit->currentMesh()->subMeshes() = m_workingMesh.subMeshes();
    }
}

void UVEditorController::applyWorkingMeshUv(int subMeshIndex, int localVert, const Ogre::Vector2& uv)
{
    if (subMeshIndex < 0 || localVert < 0)
        return;

    m_workingMesh.setVertexUV(static_cast<size_t>(subMeshIndex),
                              static_cast<size_t>(localVert), uv);

    if (m_activeEntity) {
        Ogre::Mesh* mesh = m_activeEntity->getMesh().get();
        if (mesh && static_cast<unsigned short>(subMeshIndex) < mesh->getNumSubMeshes()
            && mesh->getSubMesh(static_cast<unsigned short>(subMeshIndex))->useSharedVertices) {
            for (size_t si = 0; si < m_workingMesh.subMeshes().size(); ++si) {
                if (static_cast<int>(si) == subMeshIndex)
                    continue;
                if (si >= mesh->getNumSubMeshes() || !mesh->getSubMesh(static_cast<unsigned short>(si))->useSharedVertices)
                    continue;
                m_workingMesh.setVertexUV(si, static_cast<size_t>(localVert), uv);
            }
        }
    }

    if (auto* edit = EditModeController::instance()) {
        if (edit->isEditModeActive() && edit->editEntity() == m_activeEntity && edit->currentMesh()) {
            edit->currentMesh()->setVertexUV(static_cast<size_t>(subMeshIndex),
                                             static_cast<size_t>(localVert), uv);
            if (m_activeEntity) {
                Ogre::Mesh* mesh = m_activeEntity->getMesh().get();
                if (mesh && static_cast<unsigned short>(subMeshIndex) < mesh->getNumSubMeshes()
                    && mesh->getSubMesh(static_cast<unsigned short>(subMeshIndex))->useSharedVertices) {
                    for (size_t si = 0; si < edit->currentMesh()->subMeshes().size(); ++si) {
                        if (static_cast<int>(si) == subMeshIndex)
                            continue;
                        if (si >= mesh->getNumSubMeshes()
                            || !mesh->getSubMesh(static_cast<unsigned short>(si))->useSharedVertices)
                            continue;
                        edit->currentMesh()->setVertexUV(si, static_cast<size_t>(localVert), uv);
                    }
                }
            }
        }
    }
}

void UVEditorController::syncUvLayoutFromWorkingMesh()
{
    for (auto& vert : m_uvVerts) {
        int subIdx = 0;
        int localVert = 0;
        if (!mapGlobalVertToSubLocal(vert.meshGlobalVert, subIdx, localVert))
            continue;
        const Ogre::Vector2 uv = m_workingMesh.getVertexUV(
            static_cast<size_t>(subIdx), static_cast<size_t>(localVert));
        vert.u = uv.x;
        vert.v = uv.y;
    }

    for (auto& tri : m_uvTris) {
        for (int c = 0; c < 3; ++c) {
            const int id = tri.uvVertId[c];
            if (id < 0 || id >= static_cast<int>(m_uvVerts.size()))
                continue;
            tri.u[c] = m_uvVerts[static_cast<size_t>(id)].u;
            tri.v[c] = m_uvVerts[static_cast<size_t>(id)].v;
        }
    }

    QVariantList tris;
    tris.reserve(static_cast<int>(m_uvTris.size()));
    for (const auto& tri : m_uvTris) {
        tris.push_back(QVariantMap{
            {QStringLiteral("u0"), tri.u[0]},
            {QStringLiteral("v0"), tri.v[0]},
            {QStringLiteral("u1"), tri.u[1]},
            {QStringLiteral("v1"), tri.v[1]},
            {QStringLiteral("u2"), tri.u[2]},
            {QStringLiteral("v2"), tri.v[2]},
            {QStringLiteral("island"), tri.island},
            {QStringLiteral("color"), colorForIsland(tri.island)},
        });
    }
    m_triangles = tris;

    ++m_meshRevision;
    emit meshDataChanged();
}

void UVEditorController::updateTexturePixelSize()
{
    m_texturePixelSize = 0;
    if (!m_showTextureBackground || m_textureBackgroundSource.isEmpty())
        return;

    const QUrl url(m_textureBackgroundSource);
    const QString path = url.isLocalFile() ? url.toLocalFile() : m_textureBackgroundSource;
    QImageReader reader(path);
    if (!reader.canRead())
        return;
    const QSize size = reader.size();
    if (size.isValid())
        m_texturePixelSize = std::max(size.width(), size.height());
}

QSet<int> UVEditorController::affectedUvVertIds() const
{
    QSet<int> ids = m_selectedUvVerts;
    for (int eid : m_selectedUvEdges) {
        if (eid < 0 || eid >= m_uvEdges.size())
            continue;
        ids.insert(m_uvEdges[static_cast<size_t>(eid)].v0);
        ids.insert(m_uvEdges[static_cast<size_t>(eid)].v1);
    }
    for (int fid : m_selectedUvFaces) {
        if (fid < 0 || fid >= static_cast<int>(m_uvTris.size()))
            continue;
        const auto& tri = m_uvTris[static_cast<size_t>(fid)];
        for (int c = 0; c < 3; ++c)
            ids.insert(tri.uvVertId[c]);
    }
    return ids;
}

bool UVEditorController::mapGlobalVertToSubLocal(int globalVert, int& subMeshIndex, int& localVert) const
{
    if (globalVert < 0 || m_workingMesh.subMeshes().empty())
        return false;

    int off = 0;
    for (size_t si = 0; si < m_workingMesh.subMeshes().size(); ++si) {
        const int count = static_cast<int>(m_workingMesh.subMeshes()[si].vertices.size());
        if (globalVert >= off && globalVert < off + count) {
            subMeshIndex = static_cast<int>(si);
            localVert = globalVert - off;
            return true;
        }
        off += count;
    }
    return false;
}

UVTransform::Settings UVEditorController::transformSettings(bool invertSnap) const
{
    UVTransform::Settings settings;
    settings.pivot = static_cast<UVTransform::PivotMode>(static_cast<int>(m_pivotMode));
    settings.snap = static_cast<UVTransform::SnapMode>(static_cast<int>(m_snapMode));
    settings.snapEnabled = m_snapEnabled;
    settings.invertSnap = invertSnap;
    settings.cursor = {static_cast<float>(m_cursorU), static_cast<float>(m_cursorV)};
    settings.texturePixelSize = m_texturePixelSize;
    return settings;
}

std::vector<UVTransform::VertRef> UVEditorController::collectSelectedUvRefs() const
{
    std::vector<UVTransform::VertRef> refs;
    const QSet<int> ids = affectedUvVertIds();
    refs.reserve(ids.size());
    for (int id : ids) {
        if (id < 0 || id >= static_cast<int>(m_uvVerts.size()))
            continue;
        const auto& v = m_uvVerts[static_cast<size_t>(id)];
        refs.push_back({id, {v.u, v.v}});
    }
    return refs;
}

std::vector<UVTransform::VertRef> UVEditorController::collectAllUvRefs() const
{
    std::vector<UVTransform::VertRef> refs;
    refs.reserve(m_uvVerts.size());
    for (size_t i = 0; i < m_uvVerts.size(); ++i) {
        const auto& v = m_uvVerts[i];
        refs.push_back({static_cast<int>(i), {v.u, v.v}});
    }
    return refs;
}

bool UVEditorController::applyUvRefChanges(
    const std::vector<UVTransform::VertRef>& refs,
    const std::vector<UVTransform::VertRef>& before,
    UVTransform::TransformOp op,
    const QString& description)
{
    if (!m_activeEntity || refs.empty() || refs.size() != before.size())
        return false;

    if (m_workingMesh.subMeshes().empty())
        return false;

    std::vector<UVEditCommand::VertChange> changes;
    changes.reserve(refs.size());

    for (size_t i = 0; i < refs.size(); ++i) {
        const auto& after = refs[i];
        const auto& prev = before[i];
        if (after.id < 0 || after.id >= static_cast<int>(m_uvVerts.size()))
            continue;

        const int globalVert = m_uvVerts[static_cast<size_t>(after.id)].meshGlobalVert;
        int subIdx = 0;
        int localVert = 0;
        if (!mapGlobalVertToSubLocal(globalVert, subIdx, localVert))
            continue;

        applyWorkingMeshUv(subIdx, localVert, after.uv);

        UVEditCommand::VertChange change;
        change.subMeshIndex = subIdx;
        change.vertexIndex = localVert;
        change.oldUv = prev.uv;
        change.newUv = after.uv;
        changes.push_back(change);
    }

    if (changes.empty())
        return false;

    if (!commitWorkingMeshUvs()) {
        for (const auto& ch : changes)
            applyWorkingMeshUv(ch.subMeshIndex, ch.vertexIndex, ch.oldUv);
        return false;
    }

    if (auto* edit = EditModeController::instance()) {
        if (edit->isEditModeActive() && edit->editEntity() == m_activeEntity)
            edit->notifyMeshDataChanged();
    }

    if (UndoManager* undo = UndoManager::getSingleton())
        undo->push(new UVEditCommand(m_activeEntity, m_uvChannel, changes, description));

    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.transform"), description);
    syncUvLayoutFromWorkingMesh();
    return true;
}

Ogre::Vector2 UVEditorController::transformDeltaFromDrag(double u, double v) const
{
    const float du = static_cast<float>(u - m_dragStartU);
    const float dv = static_cast<float>(v - m_dragStartV);

    switch (m_transformMode) {
    case MoveTransform:
        return {du, dv};
    case RotateTransform:
        return {du * 180.f, 0.f};
    case ScaleTransform: {
        const float dist = std::sqrt(du * du + dv * dv);
        const float sign = (du + dv) >= 0.f ? 1.f : -1.f;
        return {1.f + sign * dist, 0.f};
    }
    default:
        return Ogre::Vector2::ZERO;
    }
}

bool UVEditorController::beginTransformDrag(double u, double v, int modifiers)
{
    if (!m_hasMesh || m_transformMode == NoTransform)
        return false;

    const auto selected = collectSelectedUvRefs();
    if (selected.empty())
        return false;

    m_dragBeforeChanges.clear();
    m_dragBeforeRefs = selected;
    for (const auto& ref : selected) {
        if (ref.id < 0 || ref.id >= static_cast<int>(m_uvVerts.size()))
            continue;
        const int globalVert = m_uvVerts[static_cast<size_t>(ref.id)].meshGlobalVert;
        int subIdx = 0;
        int localVert = 0;
        if (!mapGlobalVertToSubLocal(globalVert, subIdx, localVert))
            continue;
        UVEditCommand::VertChange change;
        change.subMeshIndex = subIdx;
        change.vertexIndex = localVert;
        change.oldUv = ref.uv;
        change.newUv = ref.uv;
        m_dragBeforeChanges.push_back(change);
    }
    if (m_dragBeforeChanges.empty())
        return false;

    m_dragStartU = u;
    m_dragStartV = v;
    m_draggingTransform = true;
    m_transformActive = true;
    emit transformActiveChanged();
    return true;
}

void UVEditorController::updateTransformDrag(double u, double v, int modifiers)
{
    if (!m_draggingTransform || m_transformMode == NoTransform)
        return;

    const bool invertSnap = (modifiers & static_cast<int>(ControlModifier)) != 0;
    const auto& before = m_dragBeforeRefs;
    UVTransform::TransformOp op = UVTransform::TransformOp::Move;
    switch (m_transformMode) {
    case MoveTransform: op = UVTransform::TransformOp::Move; break;
    case RotateTransform: op = UVTransform::TransformOp::Rotate; break;
    case ScaleTransform: op = UVTransform::TransformOp::Scale; break;
    default: return;
    }

    const Ogre::Vector2 delta = transformDeltaFromDrag(u, v);
    const auto after = UVTransform::applyTransform(
        op, before, transformSettings(invertSnap), collectAllUvRefs(), delta, 0.f, false);

    for (size_t i = 0; i < after.size() && i < before.size(); ++i) {
        const int globalVert = m_uvVerts[static_cast<size_t>(after[i].id)].meshGlobalVert;
        int subIdx = 0;
        int localVert = 0;
        if (!mapGlobalVertToSubLocal(globalVert, subIdx, localVert))
            continue;
        applyWorkingMeshUv(subIdx, localVert, after[i].uv);
    }
    commitWorkingMeshUvs();
    syncUvLayoutFromWorkingMesh();
}

void UVEditorController::commitTransformDrag()
{
    if (!m_draggingTransform)
        return;

    m_draggingTransform = false;
    m_transformActive = false;
    emit transformActiveChanged();

    if (m_dragBeforeChanges.empty() || m_dragBeforeRefs.empty())
        return;

    QString desc = tr("UV Move");
    if (m_transformMode == RotateTransform)
        desc = tr("UV Rotate");
    else if (m_transformMode == ScaleTransform)
        desc = tr("UV Scale");

    std::vector<UVEditCommand::VertChange> changes = m_dragBeforeChanges;
    for (auto& ch : changes) {
        ch.newUv = m_workingMesh.getVertexUV(
            static_cast<size_t>(ch.subMeshIndex),
            static_cast<size_t>(ch.vertexIndex));
    }

    // GPU buffers were already updated during drag; record undo only.
    if (UndoManager* undo = UndoManager::getSingleton())
        undo->push(new UVEditCommand(m_activeEntity, m_uvChannel, std::move(changes), desc));

    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.transform"), desc);

    m_dragBeforeChanges.clear();
    m_dragBeforeRefs.clear();
}

void UVEditorController::cancelTransformDrag()
{
    if (!m_draggingTransform)
        return;

    syncWorkingMeshFromEntity();
    for (const auto& ch : m_dragBeforeChanges)
        applyWorkingMeshUv(ch.subMeshIndex, ch.vertexIndex, ch.oldUv);
    commitWorkingMeshUvs();
    m_dragBeforeChanges.clear();
    m_dragBeforeRefs.clear();
    m_draggingTransform = false;
    m_transformActive = false;
    emit transformActiveChanged();
    syncUvLayoutFromWorkingMesh();
}

bool UVEditorController::applyNumericTransform(double value)
{
    if (m_transformMode == NoTransform)
        return false;

    const auto before = collectSelectedUvRefs();
    if (before.empty())
        return false;

    UVTransform::TransformOp op = UVTransform::TransformOp::Move;
    QString desc = tr("UV Move");
    if (m_transformMode == RotateTransform) {
        op = UVTransform::TransformOp::Rotate;
        desc = tr("UV Rotate");
    } else if (m_transformMode == ScaleTransform) {
        op = UVTransform::TransformOp::Scale;
        desc = tr("UV Scale");
    }

    const auto after = UVTransform::applyTransform(
        op, before, transformSettings(false), collectAllUvRefs(),
        Ogre::Vector2::ZERO, static_cast<float>(value), true);
    return applyUvRefChanges(after, before, op, desc);
}

void UVEditorController::mirrorSelectionX()
{
    const auto before = collectSelectedUvRefs();
    if (before.empty())
        return;
    const auto after = UVTransform::applyTransform(
        UVTransform::TransformOp::MirrorX, before, transformSettings(false),
        collectAllUvRefs(), Ogre::Vector2::ZERO, 0.f, false);
    applyUvRefChanges(after, before, UVTransform::TransformOp::MirrorX, tr("UV Mirror X"));
}

void UVEditorController::mirrorSelectionY()
{
    const auto before = collectSelectedUvRefs();
    if (before.empty())
        return;
    const auto after = UVTransform::applyTransform(
        UVTransform::TransformOp::MirrorY, before, transformSettings(false),
        collectAllUvRefs(), Ogre::Vector2::ZERO, 0.f, false);
    applyUvRefChanges(after, before, UVTransform::TransformOp::MirrorY, tr("UV Mirror Y"));
}

bool UVEditorController::commitWorkingMeshUvs()
{
    if (!m_activeEntity)
        return false;

    return m_workingMesh.commitUvsToEntity(m_activeEntity, m_uvChannel, nullptr);
}

void UVEditorController::refresh()
{
    m_refreshPending = false;
    m_refreshTimer->stop();
    rebuildMeshCache();
}

bool UVEditorController::readUvChannel(const Ogre::VertexData* vertexData, int channel,
                                      std::vector<Ogre::Vector2>& outUvs)
{
    outUvs.clear();
    if (!vertexData || vertexData->vertexCount == 0)
        return false;

    const auto* elem = vertexData->vertexDeclaration->findElementBySemantic(
        Ogre::VES_TEXTURE_COORDINATES, static_cast<unsigned short>(channel));
    if (!elem)
        return false;

    outUvs.resize(vertexData->vertexCount, Ogre::Vector2::ZERO);
    auto vbuf = vertexData->vertexBufferBinding->getBuffer(elem->getSource());
    if (!vbuf)
        return false;

    const size_t stride = vbuf->getVertexSize();
    auto* base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    for (size_t i = 0; i < vertexData->vertexCount; ++i) {
        float* p = nullptr;
        elem->baseVertexPointerToElement(base + i * stride, &p);
        outUvs[i] = Ogre::Vector2(p[0], p[1]);
    }
    vbuf->unlock();
    return true;
}

void UVEditorController::applyUvChannel(EditableMesh& mesh, Ogre::Entity* entity, int channel,
                                        const QSet<int>& submeshFilter)
{
    if (!entity || !entity->getMesh())
        return;

    Ogre::Mesh* ogreMesh = entity->getMesh().get();
    for (size_t si = 0; si < mesh.subMeshes().size(); ++si) {
        if (!submeshFilter.isEmpty() && !submeshFilter.contains(static_cast<int>(si)))
            continue;
        if (si >= ogreMesh->getNumSubMeshes())
            continue;

        const Ogre::SubMesh* sub = ogreMesh->getSubMesh(static_cast<unsigned short>(si));
        const Ogre::VertexData* vd = sub->useSharedVertices ? ogreMesh->sharedVertexData : sub->vertexData;
        std::vector<Ogre::Vector2> uvs;
        auto& verts = mesh.subMeshes()[si].vertices;
        if (!readUvChannel(vd, channel, uvs)) {
            for (auto& v : verts)
                v.hasUV = false;
            continue;
        }

        const size_t n = std::min(verts.size(), uvs.size());
        for (size_t vi = 0; vi < n; ++vi) {
            verts[vi].uv = uvs[vi];
            verts[vi].hasUV = true;
        }
    }
}

QString UVEditorController::colorForIsland(int islandId)
{
    const int hue = (islandId * 67) % 360;
    return QColor::fromHsv(hue, 170, 220, 200).name(QColor::HexArgb);
}

UVEditorController::IslandResult UVEditorController::computeIslandsFromHalfEdgeMesh(const HalfEdgeMesh& hem)
{
    IslandResult result;
    const int faceTotal = static_cast<int>(hem.faceCount());
    result.faceIslandIds.assign(faceTotal, -1);

    for (int start = 0; start < faceTotal; ++start) {
        if (result.faceIslandIds[start] >= 0)
            continue;

        std::queue<int> q;
        q.push(start);
        result.faceIslandIds[start] = result.islandCount;

        while (!q.empty()) {
            const int faceIdx = q.front();
            q.pop();

            const int startHE = hem.face(faceIdx).halfEdge;
            if (startHE < 0)
                continue;

            int he = startHE;
            do {
                const int twinIdx = hem.halfEdge(he).twin;
                if (twinIdx >= 0) {
                    const int adjFace = hem.halfEdge(twinIdx).face;
                    if (adjFace >= 0 && result.faceIslandIds[adjFace] < 0) {
                        result.faceIslandIds[adjFace] = result.islandCount;
                        q.push(adjFace);
                    }
                }
                he = hem.halfEdge(he).next;
            } while (he != startHE);
        }

        ++result.islandCount;
    }

    return result;
}

UVEditorController::IslandResult UVEditorController::computeIslandsFromEditableMesh(const EditableMesh& mesh)
{
    if (mesh.subMeshes().empty())
        return {};

    HalfEdgeMesh hem;
    if (!hem.buildFromEditableMesh(mesh))
        return {};

    return computeIslandsFromHalfEdgeMesh(hem);
}

namespace {

QString safePreviewBaseName(const QString& texName)
{
    QString base = QFileInfo(texName).fileName();
    base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")),
                 QStringLiteral("_"));
    if (base.isEmpty())
        base = QStringLiteral("texture");
    return base;
}

#ifdef Q_OS_MACOS
QString macAppBundleRoot()
{
    const QString bundleRoot =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../.."));
    const QString canonical = QFileInfo(bundleRoot).canonicalFilePath();
    return canonical.isEmpty() ? bundleRoot : canonical;
}
#endif

} // namespace

static QString fileUrl(const QString& path)
{
    return QUrl::fromLocalFile(path).toString();
}

static Ogre::TexturePtr findLoadedTextureByName(const std::string& name)
{
    auto texPtr = Ogre::TextureManager::getSingleton().getByName(name);
    if (texPtr)
        return texPtr;
    auto it = Ogre::TextureManager::getSingleton().getResourceIterator();
    while (it.hasMoreElements()) {
        const Ogre::ResourcePtr r = it.getNext();
        if (r && r->getName() == name)
            return Ogre::static_pointer_cast<Ogre::Texture>(r);
    }
    return {};
}

static QString diffuseTextureNameForSubEntity(Ogre::SubEntity* sub)
{
    if (!sub)
        return {};
    const Ogre::MaterialPtr mat = sub->getMaterial();
    if (!mat || mat->getNumTechniques() == 0)
        return {};
    auto* tech = mat->getTechnique(0);
    if (!tech || tech->getNumPasses() == 0)
        return {};
    auto* pass = tech->getPass(0);
    if (!pass)
        return {};

    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        const std::string n = tus->getName();
        if (n == "diffuse_map" || n == "albedo" || n == "DiffuseColor")
            return QString::fromStdString(tus->getTextureName());
    }
    if (pass->getNumTextureUnitStates() > 0)
        return QString::fromStdString(pass->getTextureUnitState(0)->getTextureName());
    return {};
}

QString UVEditorController::resolveDiffuseTextureSource(Ogre::Entity* entity, int submeshIndex)
{
    if (!entity || submeshIndex < 0
        || submeshIndex >= static_cast<int>(entity->getNumSubEntities()))
        return {};

    const QString texName = diffuseTextureNameForSubEntity(entity->getSubEntity(submeshIndex));
    if (texName.isEmpty())
        return {};

    if (auto texPtr = findLoadedTextureByName(texName.toStdString())) {
        const QString origin = QString::fromStdString(texPtr->getOrigin());
        if (!origin.isEmpty() && QFileInfo::exists(origin)) {
            SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                QStringLiteral("UV editor texture from Ogre origin: %1").arg(texName));
            return fileUrl(origin);
        }
    }

    const std::vector<uint8_t> embedded = EmbeddedTextureCache::retrieve(texName.toStdString());
    if (!embedded.empty()) {
        const QString outDir =
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                .filePath(QStringLiteral("uv_editor_previews"));
        QDir().mkpath(outDir);
        const QString outPath =
            QDir(outDir).filePath(safePreviewBaseName(texName) + QStringLiteral("_uvbg.png"));
        if (!QFileInfo::exists(outPath)) {
            QImage img;
            if (img.loadFromData(embedded.data(), static_cast<int>(embedded.size()))) {
                img.save(outPath, "PNG");
                SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
                    QStringLiteral("UV editor embedded preview: %1").arg(texName));
            }
        }
        if (QFileInfo::exists(outPath))
            return fileUrl(outPath);
    }

    QStringList candidates = {
        QStringLiteral("media/materials/textures/%1").arg(texName),
        QDir::current().filePath(texName)
    };
#ifdef Q_OS_MACOS
    candidates.prepend(QDir(macAppBundleRoot()).filePath(
        QStringLiteral("media/materials/textures/%1").arg(texName)));
    candidates.append(QDir(macAppBundleRoot()).filePath(texName));
#else
    candidates.append(QDir(QCoreApplication::applicationDirPath()).filePath(texName));
#endif
    for (const QString& path : candidates) {
        if (QFileInfo::exists(path)) {
            SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                QStringLiteral("UV editor texture from disk: %1").arg(texName));
            return fileUrl(QFileInfo(path).absoluteFilePath());
        }
    }
    return {};
}

static EditableMesh filteredEditableMesh(const EditableMesh& source, const QSet<int>& submeshFilter)
{
    EditableMesh out;
    if (submeshFilter.isEmpty()) {
        out.subMeshes() = source.subMeshes();
        return out;
    }

    for (size_t si = 0; si < source.subMeshes().size(); ++si) {
        if (submeshFilter.contains(static_cast<int>(si)))
            out.subMeshes().push_back(source.subMeshes()[si]);
    }
    return out;
}

bool UVEditorController::buildFromEntity(Ogre::Entity* entity, const QSet<int>& submeshFilter, int uvChannel)
{
    if (!entity)
        return false;

    m_uvVerts.clear();
    m_uvTris.clear();
    m_uvEdges.clear();

    EditableMesh displayMesh;
    if (auto* edit = EditModeController::instance()) {
        if (edit->isEditModeActive() && edit->editEntity() == entity && edit->currentMesh()) {
            displayMesh.subMeshes() = edit->currentMesh()->subMeshes();
        } else if (!displayMesh.loadFromEntity(entity)) {
            return false;
        }
    } else if (!displayMesh.loadFromEntity(entity)) {
        return false;
    }

    applyUvChannel(displayMesh, entity, uvChannel, submeshFilter);
    EditableMesh mesh = filteredEditableMesh(displayMesh, submeshFilter);
    if (mesh.subMeshes().empty())
        return false;

    HalfEdgeMesh hem;
    if (!hem.buildFromEditableMesh(mesh))
        return false;

    const IslandResult islands = computeIslandsFromHalfEdgeMesh(hem);

    float uMin = 1e9f, vMin = 1e9f, uMax = -1e9f, vMax = -1e9f;
    QVariantList tris;
    tris.reserve(static_cast<int>(hem.faceCount()));

    int heFaceIdx = 0;

    std::vector<size_t> sourceSubIndices;
    sourceSubIndices.reserve(displayMesh.subMeshes().size());
    if (submeshFilter.isEmpty()) {
        for (size_t si = 0; si < displayMesh.subMeshes().size(); ++si)
            sourceSubIndices.push_back(si);
    } else {
        for (size_t si = 0; si < displayMesh.subMeshes().size(); ++si) {
            if (submeshFilter.contains(static_cast<int>(si)))
                sourceSubIndices.push_back(si);
        }
    }

    auto globalVertOffsetForSub = [&](size_t sourceSubIdx) {
        int off = 0;
        for (size_t si = 0; si < sourceSubIdx; ++si)
            off += static_cast<int>(displayMesh.subMeshes()[si].vertices.size());
        return off;
    };

    auto globalTriOffsetForSub = [&](size_t sourceSubIdx) {
        int off = 0;
        for (size_t si = 0; si < sourceSubIdx; ++si)
            off += static_cast<int>(displayMesh.subMeshes()[si].triangles.size());
        return off;
    };

    for (size_t meshSubIdx = 0; meshSubIdx < mesh.subMeshes().size(); ++meshSubIdx) {
        const size_t sourceSubIdx = sourceSubIndices[meshSubIdx];
        const auto& sub = mesh.subMeshes()[meshSubIdx];
        const int globalVertOffset = globalVertOffsetForSub(sourceSubIdx);
        const int globalTriOffset = globalTriOffsetForSub(sourceSubIdx);

        auto emitTriangle = [&](const EditableTriangle& tri, int meshGlobalTri) {
            if (heFaceIdx >= static_cast<int>(islands.faceIslandIds.size()))
                return;

            UvTri uvTri;
            uvTri.meshGlobalTri = meshGlobalTri;
            uvTri.island = islands.faceIslandIds[heFaceIdx];

            Ogre::Vector2 uvs[3];
            bool ok = true;
            for (int c = 0; c < 3; ++c) {
                const size_t vi = tri.indices[c];
                if (vi >= sub.vertices.size() || !sub.vertices[vi].hasUV) {
                    ok = false;
                    break;
                }
                uvs[c] = sub.vertices[vi].uv;
                uvTri.u[c] = uvs[c].x;
                uvTri.v[c] = uvs[c].y;
                uvTri.meshGlobalVert[c] = globalVertOffset + static_cast<int>(vi);
            }
            if (!ok)
                return;

            uMin = std::min({uMin, uvs[0].x, uvs[1].x, uvs[2].x});
            vMin = std::min({vMin, uvs[0].y, uvs[1].y, uvs[2].y});
            uMax = std::max({uMax, uvs[0].x, uvs[1].x, uvs[2].x});
            vMax = std::max({vMax, uvs[0].y, uvs[1].y, uvs[2].y});

            const int islandId = islands.faceIslandIds[heFaceIdx];
            tris.push_back(QVariantMap{
                {QStringLiteral("u0"), uvs[0].x},
                {QStringLiteral("v0"), uvs[0].y},
                {QStringLiteral("u1"), uvs[1].x},
                {QStringLiteral("v1"), uvs[1].y},
                {QStringLiteral("u2"), uvs[2].x},
                {QStringLiteral("v2"), uvs[2].y},
                {QStringLiteral("island"), islandId},
                {QStringLiteral("color"), colorForIsland(islandId)}
            });
            m_uvTris.push_back(uvTri);
        };

        if (!sub.faces.empty()) {
            size_t localTriCursor = 0;
            for (const auto& face : sub.faces) {
                if (!face.isValid())
                    continue;
                const int faceBaseGlobalTri =
                    globalTriOffset + static_cast<int>(localTriCursor);
                for (size_t i = 1; i + 1 < face.indices.size(); ++i) {
                    EditableTriangle tri;
                    tri.indices[0] = face.indices[0];
                    tri.indices[1] = face.indices[i];
                    tri.indices[2] = face.indices[i + 1];
                    emitTriangle(tri, faceBaseGlobalTri);
                }
                if (face.indices.size() >= 3)
                    localTriCursor += face.indices.size() - 2;
                ++heFaceIdx;
            }
        } else {
            size_t localTri = 0;
            for (const auto& tri : sub.triangles) {
                emitTriangle(tri, globalTriOffset + static_cast<int>(localTri));
                ++localTri;
                ++heFaceIdx;
            }
        }
    }

    // Weld UV corners on shared manifold edges and build UV-edge list.
    const int cornerCount = static_cast<int>(m_uvTris.size()) * 3;
    UnionFind uf(cornerCount);

    struct EdgeOcc {
        int tri = -1;
        int c0 = -1;
        int c1 = -1;
    };
    std::unordered_map<uint64_t, std::vector<EdgeOcc>> edgeOccMap;
    edgeOccMap.reserve(m_uvTris.size() * 3);

    auto cornerIndex = [](int tri, int corner) { return tri * 3 + corner; };

    for (size_t ti = 0; ti < m_uvTris.size(); ++ti) {
        const auto& tri = m_uvTris[ti];
        for (int e = 0; e < 3; ++e) {
            const int n = (e + 1) % 3;
            int gv0 = tri.meshGlobalVert[e];
            int gv1 = tri.meshGlobalVert[n];
            if (gv0 > gv1)
                std::swap(gv0, gv1);
            const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(gv0)) << 32)
                                 | static_cast<uint32_t>(gv1);
            edgeOccMap[key].push_back({static_cast<int>(ti), e, n});
        }
    }

    for (const auto& [key, occs] : edgeOccMap) {
        if (occs.size() != 2)
            continue;
        const EdgeOcc& a = occs[0];
        const EdgeOcc& b = occs[1];
        const auto& ta = m_uvTris[a.tri];
        const auto& tb = m_uvTris[b.tri];

        auto weldCorner = [&](int triA, int cornerA, int triB, int cornerB) {
            if (std::abs(ta.u[cornerA] - tb.u[cornerB]) > kUvEpsilon
                || std::abs(ta.v[cornerA] - tb.v[cornerB]) > kUvEpsilon)
                return;
            uf.unite(cornerIndex(triA, cornerA), cornerIndex(triB, cornerB));
        };

        weldCorner(a.tri, a.c0, b.tri, b.c0);
        weldCorner(a.tri, a.c1, b.tri, b.c1);
    }

    std::unordered_map<int, int> rootToUvVert;
    rootToUvVert.reserve(cornerCount);
    for (int ci = 0; ci < cornerCount; ++ci) {
        const int tri = ci / 3;
        const int corner = ci % 3;
        const int root = uf.find(ci);
        auto it = rootToUvVert.find(root);
        if (it == rootToUvVert.end()) {
            const int vid = static_cast<int>(m_uvVerts.size());
            UvVert vert;
            vert.u = m_uvTris[tri].u[corner];
            vert.v = m_uvTris[tri].v[corner];
            vert.meshGlobalVert = m_uvTris[tri].meshGlobalVert[corner];
            m_uvVerts.push_back(vert);
            rootToUvVert.emplace(root, vid);
            it = rootToUvVert.find(root);
        }
        m_uvTris[tri].uvVertId[corner] = it->second;
    }

    std::unordered_map<uint64_t, int> edgeDedup;
    edgeDedup.reserve(m_uvTris.size() * 3);
    for (size_t ti = 0; ti < m_uvTris.size(); ++ti) {
        const auto& tri = m_uvTris[ti];
        for (int e = 0; e < 3; ++e) {
            const int n = (e + 1) % 3;
            int v0 = tri.uvVertId[e];
            int v1 = tri.uvVertId[n];
            if (v0 < 0 || v1 < 0)
                continue;
            if (v0 > v1)
                std::swap(v0, v1);
            const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(v0)) << 32)
                                 | static_cast<uint32_t>(v1);
            if (edgeDedup.find(key) != edgeDedup.end())
                continue;
            UvEdge edge;
            edge.v0 = v0;
            edge.v1 = v1;
            edge.meshGlobalV0 = tri.meshGlobalVert[e];
            edge.meshGlobalV1 = tri.meshGlobalVert[n];
            edgeDedup.emplace(key, static_cast<int>(m_uvEdges.size()));
            m_uvEdges.push_back(edge);
        }
    }

    m_trisByGlobalVert.clear();
    m_fiByGlobalTri.clear();
    int maxGlobalVert = -1;
    for (size_t fi = 0; fi < m_uvTris.size(); ++fi) {
        const auto& tri = m_uvTris[fi];
        m_fiByGlobalTri[tri.meshGlobalTri] = static_cast<int>(fi);
        for (int c = 0; c < 3; ++c) {
            maxGlobalVert = std::max(maxGlobalVert, tri.meshGlobalVert[c]);
        }
    }
    if (maxGlobalVert >= 0) {
        m_trisByGlobalVert.assign(static_cast<size_t>(maxGlobalVert + 1), {});
        for (size_t fi = 0; fi < m_uvTris.size(); ++fi) {
            const auto& tri = m_uvTris[fi];
            for (int c = 0; c < 3; ++c) {
                const int gv = tri.meshGlobalVert[c];
                if (gv >= 0)
                    m_trisByGlobalVert[static_cast<size_t>(gv)].push_back(static_cast<int>(fi));
            }
        }
    }

    m_triangles = tris;
    m_islandCount = islands.islandCount;
    m_hasMesh = !tris.isEmpty();
    if (m_hasMesh) {
        if (!std::isfinite(uMin))
            m_uvBounds = QRectF(0, 0, 1, 1);
        else
            m_uvBounds = QRectF(uMin, vMin,
                                  std::max(1e-4f, uMax - uMin),
                                  std::max(1e-4f, vMax - vMin));
    } else {
        m_uvBounds = QRectF(0, 0, 1, 1);
    }

    const int previewSub = submeshFilter.isEmpty()
        ? 0
        : *std::min_element(submeshFilter.begin(), submeshFilter.end());
    m_textureBackgroundSource = resolveDiffuseTextureSource(entity, previewSub);
    m_activeEntity = entity;

    m_sourceSubIndices.clear();
    m_subVertOffsets.clear();
    m_submeshFilter = submeshFilter;
    m_sourceSubIndices.reserve(sourceSubIndices.size());
    m_subVertOffsets.reserve(sourceSubIndices.size());
    int vertOff = 0;
    for (size_t si : sourceSubIndices) {
        m_sourceSubIndices.push_back(static_cast<int>(si));
        m_subVertOffsets.push_back(vertOff);
        vertOff += static_cast<int>(displayMesh.subMeshes()[si].vertices.size());
    }

    m_workingMesh.subMeshes() = displayMesh.subMeshes();
    applyUvChannel(m_workingMesh, entity, uvChannel, submeshFilter);
    updateTexturePixelSize();

    return m_hasMesh;
}

void UVEditorController::rebuildMeshCache()
{
    Ogre::Entity* prevEntity = m_activeEntity;
    m_triangles.clear();
    m_uvVerts.clear();
    m_uvTris.clear();
    m_uvEdges.clear();
    m_trisByGlobalVert.clear();
    m_fiByGlobalTri.clear();
    m_hasMesh = false;
    m_islandCount = 0;
    m_textureBackgroundSource.clear();
    m_uvBounds = QRectF(0, 0, 1, 1);
    m_statusText = tr("Select a mesh to view UVs.");
    m_activeEntity = nullptr;

    auto* sel = SelectionSet::getSingleton();
    if (!sel) {
        clearUvSelection();
        m_contextIslandIds.clear();
        ++m_meshRevision;
        emit meshDataChanged();
        return;
    }

    Ogre::Entity* entity = nullptr;
    QSet<int> submeshFilter;

    if (sel->hasSubEntities()) {
        for (Ogre::SubEntity* sub : sel->getSubEntitiesSelectionList()) {
            if (!sub)
                continue;
            Ogre::Entity* ent = sub->getParent();
            if (!ent)
                continue;
            if (!entity)
                entity = ent;
            if (ent != entity)
                continue;
            for (unsigned int si = 0; si < ent->getNumSubEntities(); ++si) {
                if (ent->getSubEntity(si) == sub) {
                    submeshFilter.insert(static_cast<int>(si));
                    break;
                }
            }
        }
    }

    if (!entity) {
        const auto entities = sel->getResolvedEntities();
        if (!entities.isEmpty())
            entity = entities.first();
    }

    if (!entity) {
        clearUvSelection();
        m_contextIslandIds.clear();
        ++m_meshRevision;
        emit meshDataChanged();
        return;
    }

    m_statusText = submeshFilter.isEmpty()
        ? tr("UV layout — %1").arg(QString::fromStdString(entity->getName()))
        : tr("UV layout — %1 (sub-mesh selection)").arg(QString::fromStdString(entity->getName()));

    const bool built = buildFromEntity(entity, submeshFilter, m_uvChannel);
    if (!built || entity != prevEntity)
        clearUvSelection();
    updateContextIslandsFromEdit();
    ++m_meshRevision;
    emit meshDataChanged();
}

QVariantList UVEditorController::seamEdges() const
{
    QVariantList out;
    if (!m_activeEntity || m_workingMesh.subMeshes().empty())
        return out;

    for (size_t si = 0; si < m_workingMesh.subMeshes().size(); ++si) {
        const auto& sub = m_workingMesh.subMeshes()[si];
        for (uint64_t key : sub.seamEdges) {
            const unsigned int a = static_cast<unsigned int>(key >> 32);
            const unsigned int b = static_cast<unsigned int>(key & 0xFFFFFFFFu);
            if (a >= sub.vertices.size() || b >= sub.vertices.size()
                || !sub.vertices[a].hasUV || !sub.vertices[b].hasUV)
                continue;
            QVariantMap edge;
            edge[QStringLiteral("u0")] = sub.vertices[a].uv.x;
            edge[QStringLiteral("v0")] = sub.vertices[a].uv.y;
            edge[QStringLiteral("u1")] = sub.vertices[b].uv.x;
            edge[QStringLiteral("v1")] = sub.vertices[b].uv.y;
            out.push_back(edge);
        }
    }
    return out;
}

QVariantList UVEditorController::pinnedVertices() const
{
    QVariantList out;
    if (!m_activeEntity || m_workingMesh.subMeshes().empty())
        return out;

    for (size_t si = 0; si < m_workingMesh.subMeshes().size(); ++si) {
        const auto& sub = m_workingMesh.subMeshes()[si];
        for (unsigned int vi : sub.pinnedVertices) {
            if (vi >= sub.vertices.size() || !sub.vertices[vi].hasUV)
                continue;
            QVariantMap pin;
            pin[QStringLiteral("u")] = sub.vertices[vi].uv.x;
            pin[QStringLiteral("v")] = sub.vertices[vi].uv.y;
            out.push_back(pin);
        }
    }
    return out;
}

void UVEditorController::pinSelection()
{
    if (!m_activeEntity || m_selectedUvVerts.isEmpty())
        return;

    std::vector<UvPinCommand::PinChange> changes;
    for (int vid : m_selectedUvVerts) {
        if (vid < 0 || vid >= static_cast<int>(m_uvVerts.size()))
            continue;
        const int globalVert = m_uvVerts[static_cast<size_t>(vid)].meshGlobalVert;
        int subIdx = 0;
        int localVert = 0;
        if (!mapGlobalVertToSubLocal(globalVert, subIdx, localVert))
            continue;
        auto& sub = m_workingMesh.subMeshes()[static_cast<size_t>(subIdx)];
        const bool was = UvSeamData::isPinned(sub, static_cast<unsigned int>(localVert));
        if (was)
            continue;
        UvPinCommand::PinChange ch;
        ch.subMeshIndex = static_cast<size_t>(subIdx);
        ch.vertexIndex = static_cast<unsigned int>(localVert);
        ch.oldPinned = was;
        ch.newPinned = true;
        changes.push_back(ch);
        UvSeamData::setPinned(sub, ch.vertexIndex, true);
    }
    if (changes.empty())
        return;

    if (auto* edit = EditModeController::instance()) {
        if (edit->isEditModeActive() && edit->editEntity() == m_activeEntity && edit->currentMesh())
            edit->currentMesh()->subMeshes() = m_workingMesh.subMeshes();
    }
    UvSeamData::writeBindingsToMesh(m_activeEntity->getMesh().get(), m_workingMesh.subMeshes());
    commitWorkingMeshUvs();
    if (UndoManager* undo = UndoManager::getSingleton())
        undo->push(new UvPinCommand(m_activeEntity, std::move(changes), tr("Pin UV")));
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.pin"), tr("Pin UV"));
    ++m_meshRevision;
    emit meshDataChanged();
}

void UVEditorController::unpinSelection()
{
    if (!m_activeEntity || m_selectedUvVerts.isEmpty())
        return;

    std::vector<UvPinCommand::PinChange> changes;
    for (int vid : m_selectedUvVerts) {
        if (vid < 0 || vid >= static_cast<int>(m_uvVerts.size()))
            continue;
        const int globalVert = m_uvVerts[static_cast<size_t>(vid)].meshGlobalVert;
        int subIdx = 0;
        int localVert = 0;
        if (!mapGlobalVertToSubLocal(globalVert, subIdx, localVert))
            continue;
        auto& sub = m_workingMesh.subMeshes()[static_cast<size_t>(subIdx)];
        const bool was = UvSeamData::isPinned(sub, static_cast<unsigned int>(localVert));
        if (!was)
            continue;
        UvPinCommand::PinChange ch;
        ch.subMeshIndex = static_cast<size_t>(subIdx);
        ch.vertexIndex = static_cast<unsigned int>(localVert);
        ch.oldPinned = was;
        ch.newPinned = false;
        changes.push_back(ch);
        UvSeamData::setPinned(sub, ch.vertexIndex, false);
    }
    if (changes.empty())
        return;

    if (auto* edit = EditModeController::instance()) {
        if (edit->isEditModeActive() && edit->editEntity() == m_activeEntity && edit->currentMesh())
            edit->currentMesh()->subMeshes() = m_workingMesh.subMeshes();
    }
    UvSeamData::writeBindingsToMesh(m_activeEntity->getMesh().get(), m_workingMesh.subMeshes());
    commitWorkingMeshUvs();
    if (UndoManager* undo = UndoManager::getSingleton())
        undo->push(new UvPinCommand(m_activeEntity, std::move(changes), tr("Unpin UV")));
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.pin"), tr("Unpin UV"));
    ++m_meshRevision;
    emit meshDataChanged();
}

void UVEditorController::sewSelectedEdges()
{
    if (!m_activeEntity || m_selectedUvEdges.isEmpty())
        return;

    const auto before = m_workingMesh.subMeshes();
    std::vector<std::pair<int, int>> globalEdges;
    for (int eid : m_selectedUvEdges) {
        if (eid < 0 || eid >= static_cast<int>(m_uvEdges.size()))
            continue;
        const auto& e = m_uvEdges[static_cast<size_t>(eid)];
        globalEdges.emplace_back(e.meshGlobalV0, e.meshGlobalV1);
    }
    size_t sub = 0;
    const auto keys = UvSeamOps::localEdgeKeysFromGlobal(m_workingMesh, globalEdges, sub);
    if (keys.empty())
        return;

    const auto result = UvSeamOps::sewEdges(m_workingMesh, sub, keys);
    if (!result.applied)
        return;

    const auto after = m_workingMesh.subMeshes();
    UvSeamData::writeBindingsToMesh(m_activeEntity->getMesh().get(), m_workingMesh.subMeshes());
    commitWorkingMeshUvs();
    if (UndoManager* undo = UndoManager::getSingleton())
        undo->push(new UvSeamTopologyCommand(m_activeEntity, before, after, tr("Sew UV"), false));
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.sew"), tr("Sew UV"));
    syncUvLayoutFromWorkingMesh();
    ++m_meshRevision;
    emit meshDataChanged();
}

void UVEditorController::splitSelectedEdges()
{
    if (!m_activeEntity || m_selectedUvEdges.isEmpty())
        return;

    const auto before = m_workingMesh.subMeshes();
    std::vector<std::pair<int, int>> globalEdges;
    for (int eid : m_selectedUvEdges) {
        if (eid < 0 || eid >= static_cast<int>(m_uvEdges.size()))
            continue;
        const auto& e = m_uvEdges[static_cast<size_t>(eid)];
        globalEdges.emplace_back(e.meshGlobalV0, e.meshGlobalV1);
    }
    size_t sub = 0;
    const auto keys = UvSeamOps::localEdgeKeysFromGlobal(m_workingMesh, globalEdges, sub);
    if (keys.empty())
        return;

    const auto result = UvSeamOps::splitEdges(m_workingMesh, sub, keys);
    if (!result.applied)
        return;

    const auto after = m_workingMesh.subMeshes();
    if (auto* edit = EditModeController::instance()) {
        if (edit->isEditModeActive() && edit->editEntity() == m_activeEntity && edit->currentMesh())
            edit->currentMesh()->subMeshes() = after;
    }
    m_workingMesh.subMeshes() = after;
    m_workingMesh.resizeEntityBuffers(m_activeEntity);
    UvSeamData::writeBindingsToMesh(m_activeEntity->getMesh().get(), m_workingMesh.subMeshes());
    EditModeController::rewriteEntityAfterTopologyChange(m_activeEntity);
    if (auto* edit = EditModeController::instance()) {
        if (edit->isEditModeActive() && edit->editEntity() == m_activeEntity)
            edit->notifyMeshDataChanged();
    }

    if (UndoManager* undo = UndoManager::getSingleton())
        undo->push(new UvSeamTopologyCommand(m_activeEntity, before, after, tr("Split UV")));
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.split"), tr("Split UV"));
    ++m_meshRevision;
    emit meshDataChanged();
    refresh();
}

void UVEditorController::unwrapSelectedFaces()
{
    if (!m_activeEntity || m_selectedUvFaces.isEmpty())
        return;

    commitWorkingMeshUvs();
    const auto before = m_workingMesh.subMeshes();

    UvUnwrapOptions opts;
    opts.channel = m_uvChannel;
    opts.seamEdgeKeys.resize(m_workingMesh.subMeshes().size());
    opts.pinnedUvs.resize(m_workingMesh.subMeshes().size());
    for (size_t si = 0; si < m_workingMesh.subMeshes().size(); ++si) {
        opts.seamEdgeKeys[si].assign(m_workingMesh.subMeshes()[si].seamEdges.begin(),
                                    m_workingMesh.subMeshes()[si].seamEdges.end());
        for (unsigned int vi : m_workingMesh.subMeshes()[si].pinnedVertices) {
            if (vi < m_workingMesh.subMeshes()[si].vertices.size()
                && m_workingMesh.subMeshes()[si].vertices[vi].hasUV) {
                opts.pinnedUvs[si][vi] = m_workingMesh.subMeshes()[si].vertices[vi].uv;
            }
        }
    }

    for (size_t si = 0; si < m_workingMesh.subMeshes().size(); ++si) {
        UvUnwrapOptions::FaceMask mask;
        mask.subMeshIndex = static_cast<unsigned>(si);
        mask.includeTriangle.assign(m_workingMesh.subMeshes()[si].triangles.size(), false);
        opts.faceMasks.push_back(mask);
    }

    for (int fid : m_selectedUvFaces) {
        if (fid < 0 || fid >= static_cast<int>(m_uvTris.size()))
            continue;
        const int globalTri = m_uvTris[static_cast<size_t>(fid)].meshGlobalTri;
        int offset = 0;
        bool found = false;
        int subIdx = 0;
        int localTri = 0;
        for (size_t si = 0; si < m_workingMesh.subMeshes().size(); ++si) {
            const int triCount = static_cast<int>(m_workingMesh.subMeshes()[si].triangles.size());
            if (globalTri >= offset && globalTri < offset + triCount) {
                subIdx = static_cast<int>(si);
                localTri = globalTri - offset;
                found = true;
                break;
            }
            offset += triCount;
        }
        if (!found)
            continue;
        for (auto& mask : opts.faceMasks) {
            if (mask.subMeshIndex == static_cast<unsigned>(subIdx)
                && localTri >= 0
                && static_cast<size_t>(localTri) < mask.includeTriangle.size()) {
                mask.includeTriangle[static_cast<size_t>(localTri)] = true;
            }
        }
    }

    const UvUnwrapReport report = UvUnwrap::unwrapEntity(m_activeEntity, opts);
    if (!report.applied)
        return;

    if (!m_workingMesh.loadFromEntity(m_activeEntity))
        return;
    applyUvChannel(m_workingMesh, m_activeEntity, m_uvChannel, m_submeshFilter);
    if (auto* edit = EditModeController::instance()) {
        if (edit->isEditModeActive() && edit->editEntity() == m_activeEntity && edit->currentMesh())
            edit->currentMesh()->subMeshes() = m_workingMesh.subMeshes();
    }

    const auto after = m_workingMesh.subMeshes();
    if (UndoManager* undo = UndoManager::getSingleton())
        undo->push(new UvSeamTopologyCommand(m_activeEntity, before, after,
                                             tr("Unwrap selected UVs"), true));

    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.unwrap_selected"),
                                  QStringLiteral("Unwrap selected faces"));
    clearUvSelection();
    ++m_meshRevision;
    emit meshDataChanged();
    refresh();
}
