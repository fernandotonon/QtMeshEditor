#include "LatticeController.h"

#include "EditModeController.h"
#include "EditableMesh.h"
#include "Manager.h"
#include "OgreWidget.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "SpaceCamera.h"
#include "UndoManager.h"
#include "commands/LatticeCommands.h"

#include <OgreCamera.h>
#include <OgreEntity.h>
#include <OgreManualObject.h>
#include <OgreMaterialManager.h>
#include <OgrePass.h>
#include <OgreRay.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreTechnique.h>

#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr const char* kWireMat = "Lattice/Wire";
constexpr const char* kPointMat = "Lattice/Point";
constexpr const char* kPointSelMat = "Lattice/PointSelected";
constexpr const char* kPointHoverMat = "Lattice/PointHover";
constexpr float kPickRadiusPx = 12.0f;

/// Unlit overlay material. With lighting off the fixed-function pipeline
/// shades from the VERTEX colour, so the material tracks it (TVC_DIFFUSE) and
/// every ManualObject vertex below emits its own colour — a material-level
/// diffuse alone renders white.
Ogre::MaterialPtr ensureOverlayMaterial(const char* name, float pointSize)
{
    auto& mm = Ogre::MaterialManager::getSingleton();
    Ogre::MaterialPtr mat = mm.getByName(name);
    if (mat) return mat;
    mat = mm.create(name, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
    pass->setLightingEnabled(false);
    pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
    pass->setCullingMode(Ogre::CULL_NONE);
    // Always visible over the mesh: a control point behind the surface must
    // stay grabbable (Blender's lattice edit draws its points through).
    pass->setDepthCheckEnabled(false);
    pass->setDepthWriteEnabled(false);
    pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
    if (pointSize > 0.0f) {
        pass->setPointSize(pointSize);
        pass->setPointSpritesEnabled(false);
    } else {
        pass->setLineWidth(1.5f);
    }
    return mat;
}

const Ogre::ColourValue kWireColour(0.55f, 0.75f, 1.0f, 0.7f);
const Ogre::ColourValue kPointColour(0.25f, 0.5f, 1.0f, 1.0f);
const Ogre::ColourValue kPointSelColour(1.0f, 0.62f, 0.1f, 1.0f);
const Ogre::ColourValue kPointHoverColour(1.0f, 1.0f, 1.0f, 1.0f);
} // namespace

LatticeController* LatticeController::m_pSingleton = nullptr;

LatticeController* LatticeController::instance()
{
    if (!m_pSingleton) m_pSingleton = new LatticeController();
    return m_pSingleton;
}

LatticeController* LatticeController::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    Q_UNUSED(engine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void LatticeController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

LatticeController::LatticeController() : QObject(nullptr)
{
    if (auto* sel = SelectionSet::getSingleton())
        connect(sel, &SelectionSet::selectionChanged, this, &LatticeController::selectionChanged);
    // Edit Mode owns the vertex buffers while it is active — a lattice session
    // writing the same buffers underneath it would desync EditModeController's
    // copy, so entering Edit Mode ends the session (mesh restored).
    if (auto* edit = EditModeController::instance()) {
        connect(edit, &EditModeController::editModeChanged, this, [this]() {
            emit selectionChanged();
            if (sessionActive() && EditModeController::instance()->isEditModeActive()) cancelSession();
        });
    }
    if (auto* mgr = Manager::getSingletonPtr()) {
        connect(mgr, &Manager::sceneNodeDestroyed, this, [this](Ogre::SceneNode* const& node) {
            // Emitted BEFORE the node (and our overlay child) is destroyed.
            if (sessionActive() && node && node == entityNode()) endSession();
        });
    }
}

LatticeController::~LatticeController()
{
    if (auto* um = UndoManager::getSingleton(); um && um->sessionStack() == &m_sessionUndo) um->setSessionStack(nullptr);
    destroyOverlay();
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

bool LatticeController::hasSelection() const
{
    const auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    const auto* edit = EditModeController::instance();
    if (edit && edit->isEditModeActive()) return false;
    const QList<Ogre::Entity*> ents = sel->getResolvedEntities();
    return ents.size() == 1 && ents.first() && ents.first()->getMesh();
}

Ogre::Entity* LatticeController::resolveEntity()
{
    if (!m_entity) return nullptr;
    Manager* mgr = Manager::getSingletonPtr();
    Ogre::SceneManager* scene = mgr ? mgr->getSceneMgr() : nullptr;
    // Never dereference m_entity before the SceneManager has vouched for it:
    // look the NAME up, then require the same object and the same mesh. A
    // command that replaced the entity under its node (SplitMeshCommand,
    // Explode/Join) leaves the name but not the pointer/mesh, and a new
    // allocation landing at the old address still fails the mesh check.
    Ogre::Entity* live = nullptr;
    if (scene && scene->hasEntity(m_entityName)) live = scene->getEntity(m_entityName);
    if (live && live == m_entity && live->getMesh().get() == m_meshIdentity) return live;

    // Gone or replaced: the rest snapshot no longer describes this mesh, so
    // there is nothing safe to restore — just drop the session.
    m_entity = nullptr; // endSession's overlay teardown must not touch it
    endSession();
    setStatus(tr("Lattice closed: the mesh was replaced or removed."), true);
    return nullptr;
}

Ogre::SceneNode* LatticeController::entityNode()
{
    Ogre::Entity* e = resolveEntity();
    return e ? e->getParentSceneNode() : nullptr;
}

void LatticeController::setStatus(const QString& text, bool isError)
{
    m_status = text;
    m_statusIsError = isError;
    emit statusChanged();
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

bool LatticeController::beginSession()
{
    const auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    if (const auto* edit = EditModeController::instance(); edit && edit->isEditModeActive()) {
        setStatus(tr("Leave Edit Mode before adding a lattice."), true);
        return false;
    }
    const QList<Ogre::Entity*> ents = sel->getResolvedEntities();
    if (ents.size() != 1 || !ents.first()) {
        setStatus(tr("Select exactly one mesh to deform."), true);
        return false;
    }
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("lattice.add"));
    return beginSessionOn(ents.first());
}

bool LatticeController::beginSessionOn(Ogre::Entity* entity)
{
    if (!entity || !entity->getMesh()) return false;
    if (sessionActive()) cancelSession();

    auto mesh = std::make_unique<EditableMesh>();
    if (!mesh->loadFromEntity(entity)) {
        setStatus(tr("Could not read the mesh's vertex data."), true);
        return false;
    }
    m_entity = entity;
    m_entityName = entity->getName();
    m_meshIdentity = entity->getMesh().get();
    m_mesh = std::move(mesh);
    ++m_sessionId;
    m_meshDirty = false;
    // Transient edits go on the session-local stack; UndoManager routes
    // Ctrl+Z there first while the session is open.
    m_sessionUndo.clear();
    m_sessionUndoStale = false;
    UndoManager::getSingleton()->setSessionStack(&m_sessionUndo);
    captureRest();
    m_selected.clear();
    m_hover = -1;
    rebuildRestLattice();
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.lattice.begin"),
                                  QStringLiteral("%1x%2x%3").arg(m_resX).arg(m_resY).arg(m_resZ));
    setStatus(tr("Drag the control points. Shift-click adds to the selection. Enter applies, Esc cancels."));
    emit sessionChanged();
    emit latticeChanged();
    emit pointSelectionChanged();
    return true;
}

void LatticeController::captureRest()
{
    m_rest.clear();
    if (!m_mesh) return;
    m_restNormals.clear();
    for (const auto& sm : m_mesh->subMeshes()) {
        std::vector<Ogre::Vector3> pos, nrm;
        pos.reserve(sm.vertices.size());
        nrm.reserve(sm.vertices.size());
        for (const auto& v : sm.vertices) { pos.push_back(v.position); nrm.push_back(v.normal); }
        m_rest.push_back(std::move(pos));
        m_restNormals.push_back(std::move(nrm));
    }
}

void LatticeController::rebuildRestLattice()
{
    const Lattice::Interpolation interp = m_grid.interpolation;
    Ogre::AxisAlignedBox box;
    for (const auto& sub : m_rest)
        for (const auto& p : sub) box.merge(p);
    m_grid = Lattice::Grid::fromBounds(box, m_resX, m_resY, m_resZ);
    m_grid.interpolation = interp;
    m_selected.clear();
    m_hover = -1;
    applyDeform(); // identity — restores the rest mesh if it was deformed
    emit pointSelectionChanged();
}

bool LatticeController::applySession()
{
    if (!sessionActive()) return false;
    Ogre::Entity* entity = resolveEntity();
    if (!entity || !m_mesh) { endSession(); return false; }

    if (m_grid.isAtRest()) {
        // Nothing to bake — behave like Cancel, without an empty undo step.
        cancelSession();
        setStatus(tr("Lattice removed (no deformation to apply)."));
        return true;
    }
    if (m_flushPending) flushToEntity();

    LatticeCmd::Positions deformed;
    for (const auto& sm : m_mesh->subMeshes()) {
        std::vector<Ogre::Vector3> pos;
        pos.reserve(sm.vertices.size());
        for (const auto& v : sm.vertices) pos.push_back(v.position);
        deformed.push_back(std::move(pos));
    }
    const QJsonObject json = latticeJson();
    const std::string name = m_entityName;
    const Ogre::Mesh* meshIdentity = m_meshIdentity;
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.lattice.apply"),
                                  QStringLiteral("%1x%2x%3 %4").arg(m_grid.nx).arg(m_grid.ny).arg(m_grid.nz)
                                      .arg(Lattice::interpolationId(m_grid.interpolation)));
    auto* cmd = new LatticeApplyCommand(name, meshIdentity, m_rest, m_restNormals, std::move(deformed), json,
                                        /*alreadyApplied=*/true);
    endSession(); // detaches + clears the session stack; only the bake reaches the global history
    UndoManager::getSingleton()->push(cmd);
    setStatus(tr("Lattice deformation applied."));
    return true;
}

void LatticeController::cancelSession()
{
    if (!sessionActive()) return;
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.lattice.cancel"), QString());
    restoreRestToEntity();
    endSession();
    setStatus(tr("Lattice cancelled."));
}

bool LatticeController::restoreRestToEntity()
{
    Ogre::Entity* entity = resolveEntity();
    if (!entity || !m_mesh) return false;
    m_flushPending = false;
    if (!m_meshDirty) return true; // the GPU mesh never left its rest shape
    auto& subs = m_mesh->subMeshes();
    for (size_t s = 0; s < subs.size() && s < m_rest.size(); ++s)
        for (size_t v = 0; v < subs[s].vertices.size() && v < m_rest[s].size(); ++v) {
            subs[s].vertices[v].position = m_rest[s][v];
            if (s < m_restNormals.size() && v < m_restNormals[s].size()) subs[s].vertices[v].normal = m_restNormals[s][v];
        }
    m_meshDirty = false;
    // Authored normals go back verbatim — recomputing would smooth hard edges.
    return m_mesh->commitToEntity(entity, /*recomputeNormals=*/false);
}

void LatticeController::endSession()
{
    if (auto* um = UndoManager::getSingleton(); um->sessionStack() == &m_sessionUndo) um->setSessionStack(nullptr);
    // QUndoStack::clear() deletes the commands — never under a command that
    // is currently executing (a replay that ended the session).
    if (m_inUndoReplay) m_sessionUndoStale = true;
    else m_sessionUndo.clear();
    destroyOverlay();
    m_entity = nullptr;
    m_meshIdentity = nullptr;
    m_entityName.clear();
    m_mesh.reset();
    m_rest.clear();
    m_restNormals.clear();
    m_meshDirty = false;
    m_selected.clear();
    m_hover = -1;
    m_dragActive = false;
    m_flushPending = false;
    emit sessionChanged();
    emit latticeChanged();
    emit pointSelectionChanged();
}

// ---------------------------------------------------------------------------
// Deformation
// ---------------------------------------------------------------------------

void LatticeController::applyDeform()
{
    if (!sessionActive() || !m_mesh) return;
    auto& subs = m_mesh->subMeshes();
    std::vector<Ogre::Vector3> out;
    for (size_t s = 0; s < subs.size() && s < m_rest.size(); ++s) {
        m_grid.deformAll(m_rest[s], out);
        auto& verts = subs[s].vertices;
        for (size_t v = 0; v < verts.size() && v < out.size(); ++v) verts[v].position = out[v];
    }
    refreshOverlay();
    emit latticeChanged();
    if (!sessionActive()) return; // refreshOverlay may have dropped a stale session
    // A rest lattice over a mesh that never left rest has nothing to upload
    // (and a commit would recompute the authored normals for no reason).
    if (m_grid.isAtRest() && !m_meshDirty) { m_flushPending = false; return; }
    // Coalesce GPU flushes: a drag delivers moves faster than a 90k-vertex
    // commit (normals + buffer upload) can run, so flush once per event-loop
    // turn (the vertex-paint stroke idiom).
    if (!m_flushPending) {
        m_flushPending = true;
        QTimer::singleShot(0, this, [this]() { if (m_flushPending) flushToEntity(); });
    }
}

void LatticeController::flushPendingDeform()
{
    if (m_flushPending) flushToEntity();
}

void LatticeController::flushToEntity()
{
    m_flushPending = false;
    if (m_grid.isAtRest()) { restoreRestToEntity(); return; } // back to the authored mesh, normals verbatim
    Ogre::Entity* entity = resolveEntity();
    if (!entity || !m_mesh) return;
    m_meshDirty = true;
    m_mesh->commitToEntity(entity, /*recomputeNormals=*/true); // normals follow the bend
}

bool LatticeController::deformEntityWithGrid(Ogre::Entity* entity, const Lattice::Grid& grid, QString* error)
{
    if (!entity) {
        if (error) *error = QStringLiteral("no entity");
        return false;
    }
    if (!grid.isValid()) {
        if (error) *error = QStringLiteral("invalid lattice");
        return false;
    }
    EditableMesh mesh;
    if (!mesh.loadFromEntity(entity)) {
        if (error) *error = QStringLiteral("could not read the mesh's vertex data");
        return false;
    }
    for (auto& sm : mesh.subMeshes())
        for (auto& v : sm.vertices) v.position = grid.deform(v.position);
    if (!mesh.commitToEntity(entity, /*recomputeNormals=*/true)) {
        if (error) *error = QStringLiteral("failed to write the deformed vertices");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Lattice edits
// ---------------------------------------------------------------------------

void LatticeController::setResolutionX(int n) { setResolution(n, m_resY, m_resZ); }
void LatticeController::setResolutionY(int n) { setResolution(m_resX, n, m_resZ); }
void LatticeController::setResolutionZ(int n) { setResolution(m_resX, m_resY, n); }

void LatticeController::setResolution(int nx, int ny, int nz)
{
    nx = std::clamp(nx, 2, 16); ny = std::clamp(ny, 2, 16); nz = std::clamp(nz, 2, 16);
    if (nx == m_resX && ny == m_resY && nz == m_resZ) return;
    m_resX = nx; m_resY = ny; m_resZ = nz;
    if (sessionActive()) {
        // A resolution change rebuilds a REST lattice: the old control points
        // have no meaning on the new grid, so the bend is dropped (as in
        // Blender) — but the whole previous lattice is one Ctrl+Z away.
        const QJsonObject before = m_grid.toJson();
        rebuildRestLattice();
        pushGridUndo(before, QStringLiteral("Lattice Resolution"));
        SentryReporter::addBreadcrumb(QStringLiteral("mesh.lattice.resolution"),
                                      QStringLiteral("%1x%2x%3").arg(nx).arg(ny).arg(nz));
    }
    emit latticeChanged();
}

void LatticeController::setInterpolation(int mode)
{
    mode = std::clamp(mode, 0, 2);
    const auto interp = static_cast<Lattice::Interpolation>(mode);
    if (interp == m_grid.interpolation) return;
    const QJsonObject before = m_grid.toJson();
    m_grid.interpolation = interp;
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.lattice.interpolation"), Lattice::interpolationId(interp));
    if (sessionActive()) { applyDeform(); pushGridUndo(before, QStringLiteral("Lattice Interpolation")); }
    else emit latticeChanged();
}

void LatticeController::pushGridUndo(const QJsonObject& before, const QString& description)
{
    if (!sessionActive()) return; // applyDeform may have just dropped a stale session
    const QJsonObject after = m_grid.toJson();
    if (before == after) return;
    m_sessionUndo.push(new LatticeGridCommand(m_entityName, m_sessionId, before, after, description));
}

void LatticeController::adoptGrid(const Lattice::Grid& g)
{
    const bool reshaped = g.nx != m_grid.nx || g.ny != m_grid.ny || g.nz != m_grid.nz;
    m_grid = g;
    m_resX = g.nx; m_resY = g.ny; m_resZ = g.nz;
    if (reshaped) { m_selected.clear(); m_hover = -1; }
    applyDeform();
    emit pointSelectionChanged();
}

void LatticeController::resetPoints()
{
    if (!sessionActive()) return;
    const QJsonObject before = m_grid.toJson();
    m_grid.reset();
    applyDeform();
    pushGridUndo(before, QStringLiteral("Reset Lattice"));
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.lattice.reset"), QString());
}

void LatticeController::selectAllPoints()
{
    if (!sessionActive()) return;
    for (int i = 0; i < m_grid.pointCount(); ++i) m_selected.insert(i);
    refreshOverlay();
    emit pointSelectionChanged();
}

void LatticeController::clearPointSelection()
{
    if (m_selected.empty()) return;
    m_selected.clear();
    refreshOverlay();
    emit pointSelectionChanged();
}

void LatticeController::selectPoints(const QVariantList& indices)
{
    if (!sessionActive()) return;
    m_selected.clear();
    for (const QVariant& v : indices) {
        const int i = v.toInt();
        if (i >= 0 && i < m_grid.pointCount()) m_selected.insert(i);
    }
    refreshOverlay();
    emit pointSelectionChanged();
}

QVariantList LatticeController::selectedPoints() const
{
    QVariantList out;
    for (int i : m_selected) out.append(i);
    return out;
}

QVariantList LatticeController::pointPosition(int index) const
{
    if (!sessionActive() || index < 0 || index >= m_grid.pointCount()) return {};
    const Ogre::Vector3& p = m_grid.points[static_cast<size_t>(index)];
    return {p.x, p.y, p.z};
}

QVariantList LatticeController::pointRestPosition(int index) const
{
    if (!sessionActive() || index < 0 || index >= m_grid.pointCount()) return {};
    int i, j, k;
    m_grid.coordsOf(index, i, j, k);
    const Ogre::Vector3 p = m_grid.restPoint(i, j, k);
    return {p.x, p.y, p.z};
}

void LatticeController::moveSelectedPoints(double dx, double dy, double dz)
{
    if (!sessionActive() || m_selected.empty()) return;
    const QJsonObject before = m_grid.toJson();
    const Ogre::Vector3 d(static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz));
    for (int i : m_selected) m_grid.points[static_cast<size_t>(i)] += d;
    applyDeform();
    pushGridUndo(before, QStringLiteral("Move Lattice Points"));
}

bool LatticeController::setPoint(int index, double x, double y, double z)
{
    if (!sessionActive() || index < 0 || index >= m_grid.pointCount()) return false;
    const QJsonObject before = m_grid.toJson();
    m_grid.points[static_cast<size_t>(index)] =
        Ogre::Vector3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    applyDeform();
    pushGridUndo(before, QStringLiteral("Move Lattice Point"));
    return true;
}

bool LatticeController::setPoints(const std::vector<Ogre::Vector3>& points)
{
    if (!sessionActive() || static_cast<int>(points.size()) != m_grid.pointCount()) return false;
    const QJsonObject before = m_grid.toJson();
    m_grid.points = points;
    applyDeform();
    pushGridUndo(before, QStringLiteral("Move Lattice Points"));
    return true;
}

void LatticeController::restoreGridFromUndo(const std::string& entityName, uint64_t sessionId,
                                            const QJsonObject& grid)
{
    if (!sessionActive() || entityName != m_entityName || sessionId != m_sessionId) return;
    Lattice::Grid g;
    if (!Lattice::Grid::fromJson(grid, g)) return;
    m_inUndoReplay = true;
    adoptGrid(g);
    m_inUndoReplay = false;
}

void LatticeController::abandonSessionFor(const std::string& entityName)
{
    if (!sessionActive() || entityName != m_entityName) return;
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.lattice.cancel"), QStringLiteral("abandoned (mesh rewritten)"));
    endSession();
    setStatus(tr("Lattice closed: the mesh was rewritten by another operation."), true);
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

QJsonObject LatticeController::latticeJson() const
{
    return sessionActive() ? m_grid.toJson() : QJsonObject{};
}

bool LatticeController::setLatticeJson(const QJsonObject& obj, QString* error)
{
    if (!sessionActive()) {
        if (error) *error = QStringLiteral("no lattice session");
        return false;
    }
    Lattice::Grid g;
    if (!Lattice::Grid::fromJson(obj, g, error)) return false;
    const QJsonObject before = m_grid.toJson();
    adoptGrid(g);
    pushGridUndo(before, QStringLiteral("Load Lattice"));
    return true;
}

bool LatticeController::saveLatticeToFile(const QString& path)
{
    if (!sessionActive()) { setStatus(tr("No lattice to save."), true); return false; }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatus(tr("Cannot write %1").arg(path), true);
        return false;
    }
    f.write(QJsonDocument(latticeJson()).toJson(QJsonDocument::Indented));
    if (!f.commit()) { setStatus(tr("Cannot write %1").arg(path), true); return false; }
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.lattice.save"), QString());
    setStatus(tr("Lattice saved."));
    return true;
}

bool LatticeController::loadLatticeFromFile(const QString& path)
{
    if (!sessionActive()) { setStatus(tr("Add a lattice first, then load into it."), true); return false; }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { setStatus(tr("Cannot read %1").arg(path), true); return false; }
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        setStatus(tr("Not a lattice file: %1").arg(perr.errorString()), true);
        return false;
    }
    QString err;
    if (!setLatticeJson(doc.object(), &err)) { setStatus(err, true); return false; }
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.lattice.load"), QString());
    setStatus(tr("Lattice loaded."));
    return true;
}

// ---------------------------------------------------------------------------
// Viewport interaction
// ---------------------------------------------------------------------------

bool LatticeController::screenToRay(OgreWidget* widget, const QPoint& p, Ogre::Vector3& origin, Ogre::Vector3& dir,
                                    Ogre::Camera** camOut) const
{
    if (!widget) return false;
    auto* spaceCam = widget->getSpaceCamera();
    Ogre::Camera* cam = spaceCam ? spaceCam->getCamera() : nullptr;
    if (!cam) return false;
    int vw = 0, vh = 0;
    widget->pixelSizeForCameraPicking(vw, vh);
    if (vw <= 0 || vh <= 0) return false;
    const Ogre::Ray ray = cam->getCameraToViewportRay(static_cast<Ogre::Real>(p.x()) / vw,
                                                      static_cast<Ogre::Real>(p.y()) / vh);
    origin = ray.getOrigin();
    dir = ray.getDirection();
    if (camOut) *camOut = cam;
    return true;
}

bool LatticeController::worldToScreen(const Ogre::Camera* cam, OgreWidget* widget, const Ogre::Vector3& world,
                                      float& sx, float& sy) const
{
    if (!cam || !widget) return false;
    int vw = 0, vh = 0;
    widget->pixelSizeForCameraPicking(vw, vh);
    const Ogre::Vector4 clip = cam->getProjectionMatrix() * (cam->getViewMatrix() * Ogre::Vector4(world, 1.0f));
    if (clip.w <= 1e-6f) return false; // behind the camera
    const float nx = clip.x / clip.w, ny = clip.y / clip.w;
    sx = (nx * 0.5f + 0.5f) * vw;
    sy = (1.0f - (ny * 0.5f + 0.5f)) * vh;
    return true;
}

int LatticeController::hitTestPoint(OgreWidget* widget, const QPoint& screenPos)
{
    if (!sessionActive() || !widget) return -1;
    auto* spaceCam = widget->getSpaceCamera();
    const Ogre::Camera* cam = spaceCam ? spaceCam->getCamera() : nullptr;
    Ogre::SceneNode* node = entityNode();
    if (!cam || !node) return -1;
    const Ogre::Affine3 toWorld = node->_getFullTransform();

    int best = -1;
    float bestD2 = kPickRadiusPx * kPickRadiusPx;
    float bestDepth = std::numeric_limits<float>::max();
    const Ogre::Vector3 eye = cam->getDerivedPosition();
    for (int i = 0; i < m_grid.pointCount(); ++i) {
        const Ogre::Vector3 w = toWorld * m_grid.points[static_cast<size_t>(i)];
        float sx, sy;
        if (!worldToScreen(cam, widget, w, sx, sy)) continue;
        const float dx = sx - screenPos.x(), dy = sy - screenPos.y();
        const float d2 = dx * dx + dy * dy;
        if (d2 > bestD2) continue;
        // Two points overlapping on screen (within 2 px): prefer the nearer one.
        const float depth = (w - eye).squaredLength();
        const bool overlaps = best >= 0 && std::fabs(d2 - bestD2) < 4.0f;
        if (overlaps && depth >= bestDepth) continue;
        best = i;
        bestD2 = d2;
        bestDepth = depth;
    }
    return best;
}

bool LatticeController::beginDrag(OgreWidget* widget, const QPoint& screenPos, bool additive)
{
    if (!sessionActive()) return false;
    const int hit = hitTestPoint(widget, screenPos);
    if (hit < 0) return false;
    if (additive) {
        if (m_selected.count(hit)) m_selected.erase(hit); else m_selected.insert(hit);
    } else if (!m_selected.count(hit)) {
        m_selected.clear();
        m_selected.insert(hit);
    }
    emit pointSelectionChanged();
    if (!m_selected.count(hit)) { refreshOverlay(); return false; } // toggled off — nothing to drag

    Ogre::Vector3 origin, dir;
    Ogre::Camera* cam = nullptr;
    Ogre::SceneNode* node = entityNode();
    if (!node || !screenToRay(widget, screenPos, origin, dir, &cam)) return false;
    // Drag in the camera-facing plane through the grabbed point (Blender's
    // grab). The plane is anchored at PRESS time so the point cannot drift as
    // it moves under the cursor.
    const Ogre::Vector3 hitWorld = node->_getFullTransform() * m_grid.points[static_cast<size_t>(hit)];
    m_dragPlaneNormal = cam->getDerivedDirection();
    const auto hitPlane = Ogre::Ray(origin, dir).intersects(Ogre::Plane(m_dragPlaneNormal, hitWorld));
    if (!hitPlane.first) return false;
    m_dragAnchorWorld = origin + dir * hitPlane.second;
    m_dragStartPoints = m_grid.points;
    m_dragActive = true;
    m_dragMoved = false;
    refreshOverlay();
    return true;
}

int LatticeController::selectPointsInRect(OgreWidget* widget, const QRect& screenRect, bool additive)
{
    if (!sessionActive() || !widget) return 0;
    auto* spaceCam = widget->getSpaceCamera();
    const Ogre::Camera* cam = spaceCam ? spaceCam->getCamera() : nullptr;
    Ogre::SceneNode* node = entityNode();
    if (!cam || !node) return 0;
    const QRect rect = screenRect.normalized();
    const Ogre::Affine3 toWorld = node->_getFullTransform();
    if (!additive) m_selected.clear();
    int hits = 0;
    for (int i = 0; i < m_grid.pointCount(); ++i) {
        float sx, sy;
        if (!worldToScreen(cam, widget, toWorld * m_grid.points[static_cast<size_t>(i)], sx, sy)) continue;
        if (sx < rect.left() || sx > rect.right() || sy < rect.top() || sy > rect.bottom()) continue;
        m_selected.insert(i);
        ++hits;
    }
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.lattice.box_select"), QStringLiteral("%1 points").arg(hits));
    refreshOverlay();
    emit pointSelectionChanged();
    return hits;
}

void LatticeController::updateDrag(OgreWidget* widget, const QPoint& screenPos)
{
    if (!m_dragActive || !sessionActive()) return;
    Ogre::Vector3 origin, dir;
    Ogre::SceneNode* node = entityNode();
    if (!node || !screenToRay(widget, screenPos, origin, dir)) return;
    const auto hitPlane = Ogre::Ray(origin, dir).intersects(Ogre::Plane(m_dragPlaneNormal, m_dragAnchorWorld));
    if (!hitPlane.first) return;
    const Ogre::Vector3 worldDelta = (origin + dir * hitPlane.second) - m_dragAnchorWorld;
    // World → mesh-local delta (rotation + scale, no translation).
    const Ogre::Vector3 localDelta = node->_getFullTransform().inverse().linear() * worldDelta;
    for (int i : m_selected)
        m_grid.points[static_cast<size_t>(i)] = m_dragStartPoints[static_cast<size_t>(i)] + localDelta;
    m_dragMoved = true;
    applyDeform();
}

void LatticeController::endDrag()
{
    if (!m_dragActive) return;
    m_dragActive = false;
    if (m_dragMoved && sessionActive()) {
        Lattice::Grid startGrid = m_grid;
        startGrid.points = m_dragStartPoints;
        pushGridUndo(startGrid.toJson(), QStringLiteral("Move Lattice Points"));
        SentryReporter::addBreadcrumb(QStringLiteral("mesh.lattice.drag"),
                                      QStringLiteral("%1 points").arg(m_selected.size()));
    }
    m_dragStartPoints.clear();
    refreshOverlay();
}

void LatticeController::updateHover(OgreWidget* widget, const QPoint& screenPos)
{
    if (!sessionActive() || m_dragActive) return;
    const int hit = hitTestPoint(widget, screenPos);
    if (hit == m_hover) return;
    m_hover = hit;
    refreshOverlay();
}

// ---------------------------------------------------------------------------
// Overlay
// ---------------------------------------------------------------------------

void LatticeController::refreshOverlay()
{
    if (!sessionActive()) return;
    Manager* mgr = Manager::getSingletonPtr();
    Ogre::SceneManager* scene = mgr ? mgr->getSceneMgr() : nullptr;
    Ogre::SceneNode* node = entityNode();
    if (!scene || !node) return;

    ensureOverlayMaterial(kWireMat, 0.0f);
    ensureOverlayMaterial(kPointMat, 9.0f);
    ensureOverlayMaterial(kPointSelMat, 11.0f);
    ensureOverlayMaterial(kPointHoverMat, 13.0f);

    // Dedicated child node: mesh-local geometry follows the entity transform,
    // and nothing walks the entity node's attachments expecting Entities.
    // Explicit names: Ogre registers only NAMED nodes in its lookup table, so
    // destroyOverlay's liveness checks need them.
    if (!m_overlayNode) {
        // "Unnamed_" is Manager's forbidden-name prefix: keeps the cage out of
        // the Scene tree / entity walks while still being a registered name.
        m_overlayNodeName = "Unnamed_LatticeOverlay_" + std::to_string(m_sessionId);
        m_overlayNode = scene->createSceneNode(m_overlayNodeName);
        node->addChild(m_overlayNode);
    }
    if (!m_wireObj) {
        m_wireObjName = "LatticeWire_" + std::to_string(m_sessionId);
        m_wireObj = scene->createManualObject(m_wireObjName);
        m_wireObj->setDynamic(true);
        m_wireObj->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY - 1);
        m_wireObj->setQueryFlags(0);
        m_overlayNode->attachObject(m_wireObj);
    }
    if (!m_pointObj) {
        m_pointObjName = "LatticePoints_" + std::to_string(m_sessionId);
        m_pointObj = scene->createManualObject(m_pointObjName);
        m_pointObj->setDynamic(true);
        m_pointObj->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY);
        m_pointObj->setQueryFlags(0);
        m_overlayNode->attachObject(m_pointObj);
    }

    // Wires: every grid edge along i, j, k.
    m_wireObj->clear();
    m_wireObj->begin(kWireMat, Ogre::RenderOperation::OT_LINE_LIST);
    auto P = [this](int i, int j, int k) { return m_grid.points[static_cast<size_t>(m_grid.index(i, j, k))]; };
    auto edge = [this](const Ogre::Vector3& a, const Ogre::Vector3& b) {
        m_wireObj->position(a); m_wireObj->colour(kWireColour);
        m_wireObj->position(b); m_wireObj->colour(kWireColour);
    };
    for (int k = 0; k < m_grid.nz; ++k)
        for (int j = 0; j < m_grid.ny; ++j)
            for (int i = 0; i < m_grid.nx; ++i) {
                if (i + 1 < m_grid.nx) edge(P(i, j, k), P(i + 1, j, k));
                if (j + 1 < m_grid.ny) edge(P(i, j, k), P(i, j + 1, k));
                if (k + 1 < m_grid.nz) edge(P(i, j, k), P(i, j, k + 1));
            }
    m_wireObj->end();

    // Points: three sections so selected / hovered points read at a glance.
    m_pointObj->clear();
    auto section = [this](const char* mat, const Ogre::ColourValue& colour, auto pred) {
        bool any = false;
        for (int i = 0; i < m_grid.pointCount(); ++i) if (pred(i)) { any = true; break; }
        if (!any) return;
        m_pointObj->begin(mat, Ogre::RenderOperation::OT_POINT_LIST);
        for (int i = 0; i < m_grid.pointCount(); ++i)
            if (pred(i)) { m_pointObj->position(m_grid.points[static_cast<size_t>(i)]); m_pointObj->colour(colour); }
        m_pointObj->end();
    };
    section(kPointMat, kPointColour, [this](int i) { return !m_selected.count(i) && i != m_hover; });
    section(kPointSelMat, kPointSelColour, [this](int i) { return m_selected.count(i) > 0 && i != m_hover; });
    section(kPointHoverMat, kPointHoverColour, [this](int i) { return i == m_hover; });
}

void LatticeController::destroyOverlay()
{
    Manager* mgr = Manager::getSingletonPtr();
    Ogre::SceneManager* scene = mgr ? mgr->getSceneMgr() : nullptr;
    if (scene) {
        // The overlay lives under the entity's node; if something destroyed
        // that subtree already, our pointers are stale — check by NAME first.
        Ogre::SceneNode* liveNode =
            (m_overlayNode && scene->hasSceneNode(m_overlayNodeName)) ? m_overlayNode : nullptr;
        auto destroyObj = [&](Ogre::ManualObject*& obj, const std::string& name) {
            if (!obj) return;
            if (scene->hasManualObject(name)) {
                if (liveNode) liveNode->detachObject(obj);
                scene->destroyManualObject(obj);
            }
            obj = nullptr;
        };
        destroyObj(m_wireObj, m_wireObjName);
        destroyObj(m_pointObj, m_pointObjName);
        if (liveNode) {
            if (auto* parent = liveNode->getParentSceneNode()) parent->removeChild(liveNode);
            scene->destroySceneNode(liveNode);
        }
    }
    m_wireObj = nullptr;
    m_pointObj = nullptr;
    m_overlayNode = nullptr;
}
