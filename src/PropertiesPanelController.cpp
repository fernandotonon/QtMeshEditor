#include "PropertiesPanelController.h"
#include "SceneTreeModel.h"
#include "SelectionSet.h"
#include "TransformOperator.h"
#include "PrimitiveObject.h"
#include "AnimationWidget.h"
#include "AnimationBlender.h"
#include "SkeletonTransform.h"
#include "AnimationControlController.h"
#include "AnimationMerger.h"
#include "SentryReporter.h"
#include "MeshImporterExporter.h"
#include "UndoManager.h"
#include "Manager.h"
#include "SentryReporter.h"
#include "OgreWidget.h"
#include "SpaceCamera.h"
#include "EditorViewport.h"
#include "ViewportGrid.h"
#include "ViewportSettingsKeys.h"
#include "AppSettingsKeys.h"
#include "mainwindow.h"
#include <QApplication>
#include <QFileDialog>
#include <QSettings>
#include <QPalette>
#include <Ogre.h>

PropertiesPanelController* PropertiesPanelController::m_pSingleton = nullptr;

PropertiesPanelController* PropertiesPanelController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new PropertiesPanelController();
    return m_pSingleton;
}

PropertiesPanelController* PropertiesPanelController::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine);
    Q_UNUSED(scriptEngine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void PropertiesPanelController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

PropertiesPanelController::PropertiesPanelController() : QObject(nullptr)
{
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, &PropertiesPanelController::onSelectionChanged);

    // When the blender bakes a new clip, the entity's animation list grew —
    // tell the Inspector to re-query so the new clip shows up immediately.
    connect(AnimationBlender::instance(), &AnimationBlender::clipBaked,
            this, [this]() { emit animationStateChanged(); });
    // When the blender activates, it disables every per-animation enabled
    // flag on the active entity; deactivation restores them. Either way the
    // panel needs to re-read so its checkboxes match.
    connect(AnimationBlender::instance(), &AnimationBlender::activeChanged,
            this, [this]() { emit animationStateChanged(); });

    auto* transformOp = TransformOperator::getSingleton();
    connect(transformOp, &TransformOperator::selectedPositionChanged, this, [this](const Ogre::Vector3& pos) {
        mPosX = pos.x; mPosY = pos.y; mPosZ = pos.z;
        emit transformChanged();
    });
    connect(transformOp, &TransformOperator::selectedOrientationChanged, this, [this](const Ogre::Vector3& rot) {
        mRotX = rot.x; mRotY = rot.y; mRotZ = rot.z;
        emit transformChanged();
    });
    connect(transformOp, &TransformOperator::selectedScaleChanged, this, [this](const Ogre::Vector3& scale) {
        mScaleX = scale.x; mScaleY = scale.y; mScaleZ = scale.z;
        emit transformChanged();
    });

    connect(transformOp, &TransformOperator::pivotModeChanged, this, [this]() {
        emit pivotModeChanged();
    });

    connect(transformOp, &TransformOperator::snapSettingsChanged, this, [this]() {
        emit snapSettingsChanged();
        emit snapEnabledChanged();
        emit snapGridSizeChanged();
        emit snapAngleStepChanged();
        emit snapScaleStepChanged();
    });

    connect(Manager::getSingleton(), &Manager::sceneNodeCreated, this, &PropertiesPanelController::onSceneChanged);
    connect(Manager::getSingleton(), &Manager::sceneNodeDestroyed, this, &PropertiesPanelController::onSceneChanged);

    // Undo history: refresh when the stack changes
    connect(UndoManager::getSingleton()->stack(), &QUndoStack::indexChanged,
            this, &PropertiesPanelController::undoHistoryChanged);
    connect(UndoManager::getSingleton()->stack(), &QUndoStack::cleanChanged,
            this, [this]() { emit undoHistoryChanged(); });

    mSceneTreeModel = new SceneTreeModel(this);

    // Refresh theme colors when the application palette changes (Light/Dark/Custom switch)
    connect(qApp, &QApplication::paletteChanged, this, [this]() {
        emit themeChanged();
    });
}

// Theme colors from QPalette
QColor PropertiesPanelController::panelColor() const
{
    return QApplication::palette().color(QPalette::Window);
}

QColor PropertiesPanelController::headerColor() const
{
    return QApplication::palette().color(QPalette::Window).darker(110);
}

QColor PropertiesPanelController::textColor() const
{
    return QApplication::palette().color(QPalette::WindowText);
}

QColor PropertiesPanelController::borderColor() const
{
    return QApplication::palette().color(QPalette::Mid);
}

QColor PropertiesPanelController::inputColor() const
{
    return QApplication::palette().color(QPalette::Base);
}

QColor PropertiesPanelController::controlBgColor() const
{
    // A lighter shade of the panel background — gives small toggles
    // (custom checkboxes, small buttons) a visible silhouette in dark mode
    // without being as dark as Base (which is QColor(35,35,35) and would
    // disappear next to QPalette::Window).
    return QApplication::palette().color(QPalette::Button).lighter(115);
}

QColor PropertiesPanelController::highlightColor() const
{
    return QApplication::palette().color(QPalette::Highlight);
}

// Transform accessors
double PropertiesPanelController::posX() const { return mPosX; }
double PropertiesPanelController::posY() const { return mPosY; }
double PropertiesPanelController::posZ() const { return mPosZ; }
double PropertiesPanelController::rotX() const { return mRotX; }
double PropertiesPanelController::rotY() const { return mRotY; }
double PropertiesPanelController::rotZ() const { return mRotZ; }
double PropertiesPanelController::scaleX() const { return mScaleX; }
double PropertiesPanelController::scaleY() const { return mScaleY; }
double PropertiesPanelController::scaleZ() const { return mScaleZ; }

// Transform mutators - set absolute values
void PropertiesPanelController::setPosX(double v) {
    if (mPosX != v) {
        Ogre::Vector3 pos(static_cast<Ogre::Real>(v), static_cast<Ogre::Real>(mPosY), static_cast<Ogre::Real>(mPosZ));
        TransformOperator::getSingleton()->setSelectedPosition(pos);
    }
}
void PropertiesPanelController::setPosY(double v) {
    if (mPosY != v) {
        Ogre::Vector3 pos(static_cast<Ogre::Real>(mPosX), static_cast<Ogre::Real>(v), static_cast<Ogre::Real>(mPosZ));
        TransformOperator::getSingleton()->setSelectedPosition(pos);
    }
}
void PropertiesPanelController::setPosZ(double v) {
    if (mPosZ != v) {
        Ogre::Vector3 pos(static_cast<Ogre::Real>(mPosX), static_cast<Ogre::Real>(mPosY), static_cast<Ogre::Real>(v));
        TransformOperator::getSingleton()->setSelectedPosition(pos);
    }
}
void PropertiesPanelController::setRotX(double v) {
    if (mRotX != v) {
        Ogre::Vector3 rot(static_cast<Ogre::Real>(v), static_cast<Ogre::Real>(mRotY), static_cast<Ogre::Real>(mRotZ));
        TransformOperator::getSingleton()->setSelectedOrientation(rot);
    }
}
void PropertiesPanelController::setRotY(double v) {
    if (mRotY != v) {
        Ogre::Vector3 rot(static_cast<Ogre::Real>(mRotX), static_cast<Ogre::Real>(v), static_cast<Ogre::Real>(mRotZ));
        TransformOperator::getSingleton()->setSelectedOrientation(rot);
    }
}
void PropertiesPanelController::setRotZ(double v) {
    if (mRotZ != v) {
        Ogre::Vector3 rot(static_cast<Ogre::Real>(mRotX), static_cast<Ogre::Real>(mRotY), static_cast<Ogre::Real>(v));
        TransformOperator::getSingleton()->setSelectedOrientation(rot);
    }
}
void PropertiesPanelController::setScaleX(double v) {
    if (mScaleX != v) {
        Ogre::Vector3 scale(static_cast<Ogre::Real>(v), static_cast<Ogre::Real>(mScaleY), static_cast<Ogre::Real>(mScaleZ));
        TransformOperator::getSingleton()->setSelectedScale(scale);
    }
}
void PropertiesPanelController::setScaleY(double v) {
    if (mScaleY != v) {
        Ogre::Vector3 scale(static_cast<Ogre::Real>(mScaleX), static_cast<Ogre::Real>(v), static_cast<Ogre::Real>(mScaleZ));
        TransformOperator::getSingleton()->setSelectedScale(scale);
    }
}
void PropertiesPanelController::setScaleZ(double v) {
    if (mScaleZ != v) {
        Ogre::Vector3 scale(static_cast<Ogre::Real>(mScaleX), static_cast<Ogre::Real>(mScaleY), static_cast<Ogre::Real>(v));
        TransformOperator::getSingleton()->setSelectedScale(scale);
    }
}

SceneTreeModel* PropertiesPanelController::sceneTreeModel() const
{
    return mSceneTreeModel;
}

// Selection state
bool PropertiesPanelController::hasSelection() const
{
    return !SelectionSet::getSingleton()->isEmpty();
}

bool PropertiesPanelController::hasEntitySelection() const
{
    return SelectionSet::getSingleton()->hasEntities() || SelectionSet::getSingleton()->hasSubEntities();
}

QString PropertiesPanelController::selectionName() const
{
    auto* sel = SelectionSet::getSingleton();
    if (sel->hasNodes() && sel->getNodesCount() > 0)
        return QString::fromStdString(sel->getSceneNode(0)->getName());
    if (sel->hasEntities() && sel->getEntitiesCount() > 0)
        return QString::fromStdString(sel->getEntity(0)->getName());
    return QString();
}

QStringList PropertiesPanelController::sceneNodeNames() const
{
    QStringList names;
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return names;

    for (auto* node : mgr->getSceneNodes())
        names.append(QString::fromStdString(node->getName()));
    return names;
}

// Primitive helpers
static PrimitiveObject* getSelectedPrimitive()
{
    auto* sel = SelectionSet::getSingleton();
    if (sel->hasNodes() && sel->getNodesCount() == 1)
    {
        Ogre::SceneNode* node = sel->getSceneNode(0);
        if (PrimitiveObject::isPrimitive(node))
            return PrimitiveObject::getPrimitiveFromSceneNode(node);
    }
    return nullptr;
}

QVariantList PropertiesPanelController::shortcutData() const
{
    QVariantList data;

    auto entry = [](const QString& cat, const QString& key, const QString& desc) {
        QVariantMap m;
        m["category"] = cat;
        m["key"] = key;
        m["description"] = desc;
        return m;
    };

    // Transform
    data << entry("Transform", "Q",       "Select mode");
    data << entry("Transform", "W",       "Translate mode");
    data << entry("Transform", "E",       "Rotate mode");
    data << entry("Transform", "R",       "Scale mode");
    data << entry("Transform", "X",       "Toggle World / Local space");
    data << entry("Transform", "P",       "Cycle pivot mode");

    // Navigation
    data << entry("Navigation", "F",               "Frame selection");
    data << entry("Navigation", "Middle Mouse",     "Orbit camera");
    data << entry("Navigation", "Right Mouse",      "Pan camera");
    data << entry("Navigation", "Scroll Wheel",     "Zoom camera");

    // Editing
    data << entry("Editing", "Ctrl + D",       "Duplicate selection");
    data << entry("Editing", "Ctrl + G",       "Group nodes");
    data << entry("Editing", "Ctrl + Shift + G", "Ungroup nodes");
    data << entry("Editing", "Delete",         "Remove selected");

    // Edit Mode (topology tools — active only when Tab has entered Edit Mode)
    data << entry("Edit Mode", "Tab",           "Toggle Edit Mode");
    data << entry("Edit Mode", "1 / 2 / 3",     "Vertex / Edge / Face selection");
    data << entry("Edit Mode", "M",             "Merge vertices at center");
#ifdef Q_OS_MACOS
    data << entry("Edit Mode", "Cmd + X",       "Dissolve selection");
#else
    data << entry("Edit Mode", "Ctrl + X",      "Dissolve selection");
#endif
    data << entry("Edit Mode", "X",             "Delete selection");

    // File
    data << entry("File", "Ctrl + O",       "Open scene");
    data << entry("File", "Ctrl + S",       "Save scene");
    data << entry("File", "Ctrl + Z",       "Undo");
    data << entry("File", "Ctrl + Shift + Z", "Redo");
    data << entry("File", "Ctrl + ,",       "Open preferences");

    // View
    data << entry("View", "Show Grid",       "Toggle grid display (Options menu)");
    data << entry("View", "Show Normals",    "Toggle vertex normals (Options menu)");
    data << entry("View", "Show Mesh Info",  "Toggle mesh info overlay (Options menu)");
    data << entry("View", "Show View Cube",  "Toggle 3D view cube (Options menu)");

    // Help
    data << entry("Help", "Ctrl + /", "Open keyboard shortcut reference");

    return data;
}

void PropertiesPanelController::selectNodeByName(const QString& name)
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return;

    Ogre::SceneManager* sceneMgr = mgr->getSceneMgr();
    if (sceneMgr->hasSceneNode(name.toStdString()))
    {
        Ogre::SceneNode* node = sceneMgr->getSceneNode(name.toStdString());
        SelectionSet::getSingleton()->selectOne(node);
    }
}

bool PropertiesPanelController::canReparentNode(const QString& nodeName, const QString& newParentName)
{
    if (!mSceneTreeModel) return false;
    return mSceneTreeModel->canReparent(nodeName, newParentName);
}

bool PropertiesPanelController::reparentNode(const QString& nodeName, const QString& newParentName)
{
    if (!mSceneTreeModel) return false;
    return mSceneTreeModel->reparentNode(nodeName, newParentName);
}

// ---- Undo History ----

QVariantList PropertiesPanelController::undoHistory() const
{
    QVariantList result;
    auto* stack = UndoManager::getSingleton()->stack();
    for (int i = 0; i < stack->count(); ++i)
    {
        QVariantMap entry;
        entry["text"] = stack->text(i);
        entry["isCurrent"] = (i == stack->index() - 1);
        result.append(entry);
    }
    return result;
}

int PropertiesPanelController::undoIndex() const
{
    return UndoManager::getSingleton()->stack()->index();
}

void PropertiesPanelController::undoToIndex(int index)
{
    auto* stack = UndoManager::getSingleton()->stack();
    if (index < 0 || index > stack->count()) return;

    SentryReporter::addBreadcrumb("ui.action",
        QString("Undo history jump to index %1").arg(index));

    stack->setIndex(index);
}

void PropertiesPanelController::clearUndoHistory()
{
    SentryReporter::addBreadcrumb("ui.action", "Clear undo history");
    UndoManager::getSingleton()->clear();
    emit undoHistoryChanged();
}

bool PropertiesPanelController::hasPrimitive() const { return getSelectedPrimitive() != nullptr; }

QString PropertiesPanelController::primitiveType() const
{
    auto* p = getSelectedPrimitive();
    if (!p) return QString();
    switch (p->getType()) {
    case PrimitiveObject::AP_CUBE:       return "Cube";
    case PrimitiveObject::AP_SPHERE:     return "Sphere";
    case PrimitiveObject::AP_PLANE:      return "Plane";
    case PrimitiveObject::AP_CYLINDER:   return "Cylinder";
    case PrimitiveObject::AP_CONE:       return "Cone";
    case PrimitiveObject::AP_TORUS:      return "Torus";
    case PrimitiveObject::AP_TUBE:       return "Tube";
    case PrimitiveObject::AP_CAPSULE:    return "Capsule";
    case PrimitiveObject::AP_ICOSPHERE:  return "IcoSphere";
    case PrimitiveObject::AP_ROUNDEDBOX: return "Rounded Box";
    case PrimitiveObject::AP_SPRING:     return "Spring";
    default: return "Mesh";
    }
}

double PropertiesPanelController::primSizeX() const { auto* p = getSelectedPrimitive(); return p ? p->getSizeX() : 0; }
double PropertiesPanelController::primSizeY() const { auto* p = getSelectedPrimitive(); return p ? p->getSizeY() : 0; }
double PropertiesPanelController::primSizeZ() const { auto* p = getSelectedPrimitive(); return p ? p->getSizeZ() : 0; }
double PropertiesPanelController::primRadius() const { auto* p = getSelectedPrimitive(); return p ? p->getRadius() : 0; }
double PropertiesPanelController::primRadius2() const { auto* p = getSelectedPrimitive(); return p ? p->getInnerRadius() : 0; }
double PropertiesPanelController::primHeight() const { auto* p = getSelectedPrimitive(); return p ? p->getHeight() : 0; }
int PropertiesPanelController::primSegX() const { auto* p = getSelectedPrimitive(); return p ? p->getNumSegX() : 0; }
int PropertiesPanelController::primSegY() const { auto* p = getSelectedPrimitive(); return p ? p->getNumSegY() : 0; }
int PropertiesPanelController::primSegZ() const { auto* p = getSelectedPrimitive(); return p ? p->getNumSegZ() : 0; }
double PropertiesPanelController::primUTile() const { auto* p = getSelectedPrimitive(); return p ? p->getUTile() : 1; }
double PropertiesPanelController::primVTile() const { auto* p = getSelectedPrimitive(); return p ? p->getVTile() : 1; }

void PropertiesPanelController::setPrimSizeX(double v) { auto* p = getSelectedPrimitive(); if (p) { p->setSizeX(v); emit primitiveChanged(); } }
void PropertiesPanelController::setPrimSizeY(double v) { auto* p = getSelectedPrimitive(); if (p) { p->setSizeY(v); emit primitiveChanged(); } }
void PropertiesPanelController::setPrimSizeZ(double v) { auto* p = getSelectedPrimitive(); if (p) { p->setSizeZ(v); emit primitiveChanged(); } }
void PropertiesPanelController::setPrimRadius(double v) { auto* p = getSelectedPrimitive(); if (p) { p->setRadius(v); emit primitiveChanged(); } }
void PropertiesPanelController::setPrimRadius2(double v) { auto* p = getSelectedPrimitive(); if (p) { p->setInnerRadius(v); emit primitiveChanged(); } }
void PropertiesPanelController::setPrimHeight(double v) { auto* p = getSelectedPrimitive(); if (p) { p->setHeight(v); emit primitiveChanged(); } }

QVariantMap PropertiesPanelController::primFieldConfig() const
{
    QVariantMap cfg;
    auto* p = getSelectedPrimitive();
    if (!p) return cfg;

    auto t = p->getType();

    // Which fields are visible
    cfg["showSizeX"]   = (t == PrimitiveObject::AP_CUBE || t == PrimitiveObject::AP_PLANE || t == PrimitiveObject::AP_ROUNDEDBOX);
    cfg["showSizeY"]   = (t == PrimitiveObject::AP_CUBE || t == PrimitiveObject::AP_PLANE || t == PrimitiveObject::AP_ROUNDEDBOX);
    cfg["showSizeZ"]   = (t == PrimitiveObject::AP_CUBE || t == PrimitiveObject::AP_ROUNDEDBOX);
    cfg["showRadius"]  = (t == PrimitiveObject::AP_SPHERE || t == PrimitiveObject::AP_CYLINDER || t == PrimitiveObject::AP_CONE
                       || t == PrimitiveObject::AP_TORUS || t == PrimitiveObject::AP_TUBE || t == PrimitiveObject::AP_CAPSULE
                       || t == PrimitiveObject::AP_ICOSPHERE || t == PrimitiveObject::AP_ROUNDEDBOX);
    cfg["showRadius2"] = (t == PrimitiveObject::AP_TORUS || t == PrimitiveObject::AP_TUBE);
    cfg["showHeight"]  = (t == PrimitiveObject::AP_CYLINDER || t == PrimitiveObject::AP_CONE || t == PrimitiveObject::AP_TUBE || t == PrimitiveObject::AP_CAPSULE);
    cfg["showSegX"]    = true; // all primitives have at least segX
    cfg["showSegY"]    = (t == PrimitiveObject::AP_CUBE || t == PrimitiveObject::AP_SPHERE || t == PrimitiveObject::AP_PLANE
                       || t == PrimitiveObject::AP_TORUS || t == PrimitiveObject::AP_CAPSULE || t == PrimitiveObject::AP_ROUNDEDBOX
                       || t == PrimitiveObject::AP_SPRING);
    cfg["showSegZ"]    = (t == PrimitiveObject::AP_CUBE || t == PrimitiveObject::AP_CYLINDER || t == PrimitiveObject::AP_CONE
                       || t == PrimitiveObject::AP_TUBE || t == PrimitiveObject::AP_CAPSULE || t == PrimitiveObject::AP_ROUNDEDBOX);
    cfg["showUV"]      = (t != PrimitiveObject::AP_SPRING);

    // Labels
    switch (t) {
    case PrimitiveObject::AP_SPHERE:
        cfg["radiusLabel"] = "Radius"; cfg["segXLabel"] = "Ring"; cfg["segYLabel"] = "Loop"; break;
    case PrimitiveObject::AP_CYLINDER:
    case PrimitiveObject::AP_CONE:
        cfg["radiusLabel"] = "Radius"; cfg["segXLabel"] = "Base"; cfg["segZLabel"] = "Height"; break;
    case PrimitiveObject::AP_TORUS:
        cfg["radiusLabel"] = "Radius"; cfg["radius2Label"] = "Section R"; cfg["segXLabel"] = "Circle"; cfg["segYLabel"] = "Section"; break;
    case PrimitiveObject::AP_TUBE:
        cfg["radiusLabel"] = "Outer R"; cfg["radius2Label"] = "Inner R"; cfg["segXLabel"] = "Base"; cfg["segZLabel"] = "Height"; break;
    case PrimitiveObject::AP_CAPSULE:
        cfg["radiusLabel"] = "Radius"; cfg["segXLabel"] = "Ring"; cfg["segYLabel"] = "Loop"; cfg["segZLabel"] = "Height"; break;
    case PrimitiveObject::AP_ICOSPHERE:
        cfg["radiusLabel"] = "Radius"; cfg["segXLabel"] = "Iterations"; break;
    case PrimitiveObject::AP_ROUNDEDBOX:
        cfg["radiusLabel"] = "Chamfer"; cfg["segXLabel"] = "X"; cfg["segYLabel"] = "Y"; cfg["segZLabel"] = "Z"; break;
    case PrimitiveObject::AP_SPRING:
        cfg["segXLabel"] = "Circle"; cfg["segYLabel"] = "Path"; break;
    default:
        cfg["segXLabel"] = "X"; cfg["segYLabel"] = "Y"; cfg["segZLabel"] = "Z"; break;
    }

    return cfg;
}
void PropertiesPanelController::setPrimSegX(int v) { auto* p = getSelectedPrimitive(); if (p) { p->setNumSegX(v); emit primitiveChanged(); } }
void PropertiesPanelController::setPrimSegY(int v) { auto* p = getSelectedPrimitive(); if (p) { p->setNumSegY(v); emit primitiveChanged(); } }
void PropertiesPanelController::setPrimSegZ(int v) { auto* p = getSelectedPrimitive(); if (p) { p->setNumSegZ(v); emit primitiveChanged(); } }
void PropertiesPanelController::setPrimUTile(double v) { auto* p = getSelectedPrimitive(); if (p) { p->setUTile(v); emit primitiveChanged(); } }
void PropertiesPanelController::setPrimVTile(double v) { auto* p = getSelectedPrimitive(); if (p) { p->setVTile(v); emit primitiveChanged(); } }

bool PropertiesPanelController::hasAnimations() const
{
    auto entities = SelectionSet::getSingleton()->getResolvedEntities();
    for (Ogre::Entity* ent : entities)
    {
        auto* states = ent->getAllAnimationStates();
        if (states && !states->getAnimationStates().empty())
            return true;
    }
    return false;
}

QVariantList PropertiesPanelController::animationData() const
{
    QVariantList result;
    auto entities = SelectionSet::getSingleton()->getResolvedEntities();
    for (Ogre::Entity* ent : entities)
    {
        auto* states = ent->getAllAnimationStates();
        if (!states || states->getAnimationStates().empty()) continue;

        QVariantMap entityGroup;
        entityGroup["entity"] = QString::fromStdString(ent->getName());
        entityGroup["hasSkeleton"] = ent->hasSkeleton();
        entityGroup["showSkeleton"] = mAnimationWidget ? mAnimationWidget->isSkeletonDebugActive(ent) : false;
        entityGroup["showWeights"] = mAnimationWidget ? mAnimationWidget->isBoneWeightsShown(ent) : false;

        QVariantList anims;
        for (const auto& [key, state] : states->getAnimationStates())
        {
            QVariantMap anim;
            anim["name"] = QString::fromStdString(key);
            anim["enabled"] = state->getEnabled();
            anim["loop"] = state->getLoop();
            anim["length"] = state->getLength();
            anims.append(anim);
        }
        entityGroup["animations"] = anims;
        result.append(entityGroup);
    }
    return result;
}

void PropertiesPanelController::toggleAnimationEnabled(const QString& entityName, const QString& animName, bool enabled)
{
    auto entities = SelectionSet::getSingleton()->getResolvedEntities();
    for (Ogre::Entity* ent : entities)
    {
        if (QString::fromStdString(ent->getName()) != entityName) continue;
        auto* state = ent->getAnimationState(animName.toStdString());
        if (state)
        {
            state->setEnabled(enabled);
            if (enabled) state->setLoop(true);
            emit animationStateChanged();
        }
    }
}

void PropertiesPanelController::toggleAnimationLoop(const QString& entityName, const QString& animName, bool loop)
{
    auto entities = SelectionSet::getSingleton()->getResolvedEntities();
    for (Ogre::Entity* ent : entities)
    {
        if (QString::fromStdString(ent->getName()) != entityName) continue;
        auto* state = ent->getAnimationState(animName.toStdString());
        if (state)
        {
            state->setLoop(loop);
            emit animationStateChanged();
        }
    }
}

void PropertiesPanelController::setPlaying(bool playing)
{
    if (mPlaying != playing)
    {
        mPlaying = playing;
        emit playingChanged();
    }
}

void PropertiesPanelController::toggleSkeletonDebug(const QString& entityName, bool show)
{
    if (!mAnimationWidget) return;
    auto entities = SelectionSet::getSingleton()->getResolvedEntities();
    for (Ogre::Entity* ent : entities)
    {
        if (QString::fromStdString(ent->getName()) == entityName)
        {
            mAnimationWidget->toggleSkeletonDebug(ent, show);
            emit animationStateChanged();
            return;
        }
    }
}

void PropertiesPanelController::toggleBoneWeights(const QString& entityName, bool show)
{
    if (!mAnimationWidget) return;
    auto entities = SelectionSet::getSingleton()->getResolvedEntities();
    for (Ogre::Entity* ent : entities)
    {
        if (QString::fromStdString(ent->getName()) == entityName)
        {
            mAnimationWidget->toggleBoneWeights(ent, show);
            emit animationStateChanged();
            return;
        }
    }
}

bool PropertiesPanelController::renameAnimation(const QString& entityName, const QString& oldName, const QString& newName)
{
    if (newName.isEmpty() || oldName == newName) return false;

    auto entities = SelectionSet::getSingleton()->getResolvedEntities();
    for (Ogre::Entity* ent : entities)
    {
        if (QString::fromStdString(ent->getName()) != entityName) continue;
        if (Manager::getSingleton()->hasAnimationName(ent, newName)) return false;

        // Disable skeleton debug/weights and stop playback before rename
        // (rename recreates entity internals, stale pointers would crash)
        if (mAnimationWidget)
        {
            // disableAllSkeletonDebug is private, but toggleSkeletonDebug(false) cleans up
            if (mAnimationWidget->isSkeletonDebugActive(ent))
                mAnimationWidget->toggleSkeletonDebug(ent, false);
            if (mAnimationWidget->isBoneWeightsShown(ent))
                mAnimationWidget->toggleBoneWeights(ent, false);
        }
        setPlaying(false);
        if (auto* animSet = ent->getAllAnimationStates())
        {
            for (const auto& [key, state] : animSet->getAnimationStates())
                state->setEnabled(false);
        }

        if (SkeletonTransform::renameAnimation(ent, oldName, newName))
        {
            // Re-select current selection to force all widgets (including the
            // old AnimationWidget) to refresh their tables with new entity data.
            // This prevents stale pointers from crashing the poll timer.
            auto nodes = SelectionSet::getSingleton()->getNodesSelectionList();
            SelectionSet::getSingleton()->clear();
            for (auto* node : nodes)
                SelectionSet::getSingleton()->append(node);

            emit animationStateChanged();
            return true;
        }
    }
    return false;
}

// Forwards to AnimationMerger::tolerancesForPreset — single source of truth
// shared by CLI, MCP and Inspector.
static AnimationMerger::SimplifyTolerances tolerancesForPreset(const QString& preset)
{
    return AnimationMerger::tolerancesForPreset(preset.toStdString());
}

QVariantMap PropertiesPanelController::analyzeAnimationKeyframes(const QString& entityName,
                                                                 const QString& animName,
                                                                 const QString& preset)
{
    QVariantMap result;
    result["total"] = 0;
    result["redundant"] = 0;
    result["percent"] = 0.0;

    auto entities = SelectionSet::getSingleton()->getResolvedEntities();
    for (Ogre::Entity* ent : entities) {
        if (QString::fromStdString(ent->getName()) != entityName) continue;
        if (!ent->hasSkeleton()) return result;
        Ogre::SkeletonPtr skel = ent->getMesh()->getSkeleton();
        if (!skel || !skel->hasAnimation(animName.toStdString())) return result;

        int total = 0;
        int redundant = 0;
        AnimationMerger::analyzeRedundantKeyframes(
            skel->getAnimation(animName.toStdString()),
            tolerancesForPreset(preset),
            &total, &redundant);
        result["total"] = total;
        result["redundant"] = redundant;
        result["percent"] = total > 0 ? (100.0 * redundant / total) : 0.0;
        return result;
    }
    return result;
}

int PropertiesPanelController::simplifyAnimation(const QString& entityName,
                                                 const QString& animName,
                                                 const QString& preset)
{
    auto entities = SelectionSet::getSingleton()->getResolvedEntities();
    for (Ogre::Entity* ent : entities) {
        if (QString::fromStdString(ent->getName()) != entityName) continue;
        if (!ent->hasSkeleton()) return 0;
        Ogre::SkeletonPtr skel = ent->getMesh()->getSkeleton();
        if (!skel || !skel->hasAnimation(animName.toStdString())) return 0;

        // Stop playback and disable debug overlays before mutating skeleton
        // tracks — same precaution as renameAnimation, since we recreate the
        // animation under the hood.
        if (mAnimationWidget) {
            if (mAnimationWidget->isSkeletonDebugActive(ent))
                mAnimationWidget->toggleSkeletonDebug(ent, false);
            if (mAnimationWidget->isBoneWeightsShown(ent))
                mAnimationWidget->toggleBoneWeights(ent, false);
        }
        setPlaying(false);
        if (auto* animSet = ent->getAllAnimationStates()) {
            for (const auto& [key, state] : animSet->getAnimationStates())
                state->setEnabled(false);
        }

        const int removed = AnimationMerger::simplifyAnimation(
            skel.get(), animName.toStdString(), tolerancesForPreset(preset));

        SentryReporter::addBreadcrumb("ui.action",
            QString("Simplify animation '%1' (%2): removed %3 keyframes")
                .arg(animName, preset).arg(removed));

        ent->refreshAvailableAnimationState();

        // Re-select to refresh widget tables (avoids stale pointers).
        auto nodes = SelectionSet::getSingleton()->getNodesSelectionList();
        SelectionSet::getSingleton()->clear();
        for (auto* node : nodes)
            SelectionSet::getSingleton()->append(node);

        emit animationStateChanged();
        return removed;
    }
    return 0;
}

namespace {
// Channel ids exposed by the controller's per-bone bake API. Each
// of the 10 TRS components is its own callable channel; bake-all
// runs through this list once per bone.
constexpr const char* kAllChannels[] = {
    "tx", "ty", "tz",
    "rw", "rx", "ry", "rz",
    "sx", "sy", "sz"
};
} // namespace

int PropertiesPanelController::bakeAnimation(const QString& entityName,
                                             const QString& animName,
                                             int density)
{
    auto entities = SelectionSet::getSingleton()->getResolvedEntities();
    for (Ogre::Entity* ent : entities) {
        if (QString::fromStdString(ent->getName()) != entityName) continue;
        if (!ent->hasSkeleton()) return 0;
        Ogre::SkeletonInstance* skel = ent->getSkeleton();
        if (!skel || !skel->hasAnimation(animName.toStdString())) return 0;
        Ogre::Animation* anim = skel->getAnimation(animName.toStdString());

        // Stop playback so the bake doesn't fight running advancement.
        if (mAnimationWidget) {
            if (mAnimationWidget->isSkeletonDebugActive(ent))
                mAnimationWidget->toggleSkeletonDebug(ent, false);
        }
        setPlaying(false);

        // The bake API lives on AnimationControlController and reads
        // its m_selectedSkeleton / m_selectedAnimation members, so
        // route through selectAnimation to set those without
        // disturbing playback further.
        auto* animCtrl = AnimationControlController::instance();
        const QString prevEntity = animCtrl->selectedEntityName();
        const QString prevAnim   = animCtrl->selectedAnimation();
        animCtrl->selectAnimation(entityName, animName);

        auto* stack = UndoManager::getSingleton()->stack();
        stack->beginMacro(QObject::tr("Bake animation"));
        int trackCount = 0;
        for (const auto& [handle, track] : anim->_getNodeTrackList()) {
            Ogre::Node* node = track->getAssociatedNode();
            if (!node) continue;
            const QString boneName = QString::fromStdString(node->getName());
            for (const char* ch : kAllChannels) {
                if (animCtrl->resampleAllSegmentsForBone(
                        boneName, QString::fromUtf8(ch), density) > 0) {
                    ++trackCount;
                }
            }
        }
        stack->endMacro();

        // Restore the prior selection so the user's panel state stays put.
        if (prevEntity != entityName || prevAnim != animName) {
            animCtrl->selectAnimation(prevEntity, prevAnim);
        }

        SentryReporter::addBreadcrumb("ui.action",
            QString("Bake animation '%1' (density %2): %3 tracks")
                .arg(animName).arg(density).arg(trackCount));
        emit animationStateChanged();
        return trackCount;
    }
    return 0;
}

int PropertiesPanelController::reduceAnimationToFps(const QString& entityName,
                                                     const QString& animName,
                                                     int targetFps)
{
    if (targetFps <= 0) return 0;
    auto entities = SelectionSet::getSingleton()->getResolvedEntities();
    for (Ogre::Entity* ent : entities) {
        if (QString::fromStdString(ent->getName()) != entityName) continue;
        if (!ent->hasSkeleton()) return 0;
        Ogre::SkeletonInstance* skel = ent->getSkeleton();
        if (!skel || !skel->hasAnimation(animName.toStdString())) return 0;
        Ogre::Animation* anim = skel->getAnimation(animName.toStdString());

        setPlaying(false);

        auto* animCtrl = AnimationControlController::instance();
        const QString prevEntity = animCtrl->selectedEntityName();
        const QString prevAnim   = animCtrl->selectedAnimation();
        animCtrl->selectAnimation(entityName, animName);

        auto* stack = UndoManager::getSingleton()->stack();
        stack->beginMacro(QObject::tr("Reduce animation"));
        int totalRemoved = 0;
        for (const auto& [handle, track] : anim->_getNodeTrackList()) {
            Ogre::Node* node = track->getAssociatedNode();
            if (!node) continue;
            const QString boneName = QString::fromStdString(node->getName());
            totalRemoved += animCtrl->reduceTrackToFps(boneName, targetFps);
        }
        stack->endMacro();

        if (prevEntity != entityName || prevAnim != animName) {
            animCtrl->selectAnimation(prevEntity, prevAnim);
        }

        SentryReporter::addBreadcrumb("ui.action",
            QString("Reduce animation '%1' to %2 FPS: %3 keyframes removed")
                .arg(animName).arg(targetFps).arg(totalRemoved));
        emit animationStateChanged();
        return totalRemoved;
    }
    return 0;
}

bool PropertiesPanelController::exportCurrentPose(const QString& path)
{
    auto entities = SelectionSet::getSingleton()->getResolvedEntities();
    Ogre::Entity* animatedEntity = nullptr;
    for (Ogre::Entity* ent : entities)
    {
        if (ent->hasSkeleton()) {
            animatedEntity = ent;
            break;
        }
    }

    if (!animatedEntity) return false;

    QString outputPath = path;
    if (outputPath.isEmpty())
    {
        QString filter = "STL (*.stl)";
        outputPath = QFileDialog::getSaveFileName(
            nullptr,
            QObject::tr("Export Current Pose"),
            QString::fromStdString(animatedEntity->getName()) + "_pose",
            MeshImporterExporter::exportFileDialogFilter(),
            &filter,
            QFileDialog::DontUseNativeDialog);
        if (outputPath.isEmpty()) return false;

        outputPath = MeshImporterExporter::formatFileURI(outputPath, filter);
    }

    SentryReporter::addBreadcrumb("ui.action", "Export current pose via Inspector");
    int result = MeshImporterExporter::exportCurrentPose(animatedEntity, outputPath);
    return result == 0;
}

void PropertiesPanelController::onSelectionChanged()
{
    onTransformChanged();
    emit selectionChanged();
    emit primitiveChanged();
}

void PropertiesPanelController::onTransformChanged()
{
    emit transformChanged();
}

void PropertiesPanelController::onSceneChanged()
{
    emit sceneChanged();
}

void PropertiesPanelController::refreshTheme()
{
    emit themeChanged();
}

// Pivot mode — delegate to TransformOperator
int PropertiesPanelController::pivotMode() const
{
    return static_cast<int>(TransformOperator::getSingleton()->pivotMode());
}

void PropertiesPanelController::setPivotMode(int mode)
{
    if (mode >= TransformOperator::PIVOT_CENTER && mode <= TransformOperator::PIVOT_ORIGIN)
    {
        TransformOperator::getSingleton()->setPivotMode(
            static_cast<TransformOperator::PivotMode>(mode));
        emit pivotModeChanged();
    }
}

void PropertiesPanelController::cyclePivotMode()
{
    TransformOperator::getSingleton()->cyclePivotMode();
    emit pivotModeChanged();
}

// Snap settings — delegate to TransformOperator
bool PropertiesPanelController::snapEnabled() const
{
    return TransformOperator::getSingleton()->isSnapEnabled();
}

double PropertiesPanelController::snapGridSize() const
{
    return TransformOperator::getSingleton()->snapGridSize();
}

double PropertiesPanelController::snapAngleStep() const
{
    return TransformOperator::getSingleton()->snapAngleStep();
}

double PropertiesPanelController::snapScaleStep() const
{
    return TransformOperator::getSingleton()->snapScaleStep();
}

void PropertiesPanelController::setSnapEnabled(bool enabled)
{
    TransformOperator::getSingleton()->setSnapEnabled(enabled);
    emit snapEnabledChanged();
}

void PropertiesPanelController::setSnapGridSize(double size)
{
    TransformOperator::getSingleton()->setSnapGridSize(size);
    emit snapGridSizeChanged();
}

void PropertiesPanelController::setSnapAngleStep(double degrees)
{
    TransformOperator::getSingleton()->setSnapAngleStep(degrees);
    emit snapAngleStepChanged();
}

void PropertiesPanelController::setSnapScaleStep(double step)
{
    TransformOperator::getSingleton()->setSnapScaleStep(step);
    emit snapScaleStepChanged();
}

QVariantList PropertiesPanelController::gridSizePresets() const
{
    QVariantList result;
    for (double v : TransformOperator::gridSizePresets())
        result.append(v);
    return result;
}

QVariantList PropertiesPanelController::angleStepPresets() const
{
    QVariantList result;
    for (double v : TransformOperator::angleStepPresets())
        result.append(v);
    return result;
}

QVariantList PropertiesPanelController::scaleStepPresets() const
{
    QVariantList result;
    for (double v : TransformOperator::scaleStepPresets())
        result.append(v);
    return result;
}

namespace
{
void applyGridVisibleFromSettings(const QVariant& value)
{
    auto* grid = Manager::getSingleton()->getViewportGrid();
    if (grid) {
        grid->setVisible(value.toBool());
    }
}

void applyCameraSpeedToAllViewports(Ogre::Real speed)
{
    if (speed <= 0) {
        speed = 0.5f;
    }
    auto* activeWidget = TransformOperator::getSingleton()->getActiveWidget();
    if (activeWidget && activeWidget->getSpaceCamera()) {
        activeWidget->getSpaceCamera()->setCameraSpeed(speed);
    }
    auto* mainWin = Manager::getSingleton()->getMainWindow();
    if (!mainWin) {
        return;
    }
    for (auto* ow : mainWin->findChildren<OgreWidget*>()) {
        if (ow->getSpaceCamera()) {
            ow->getSpaceCamera()->setCameraSpeed(speed);
        }
    }
}

void applyClipPlaneToAllViewports(const QString& key, const QVariant& value)
{
    const bool isNear = (key == ViewportSettingsKeys::nearClip());
    const Ogre::Real v = value.toReal();
    auto* mainWin = Manager::getSingleton()->getMainWindow();
    if (!mainWin) {
        return;
    }
    for (auto* ow : mainWin->findChildren<OgreWidget*>()) {
        Ogre::Camera* cam = nullptr;
        if (ow->getSpaceCamera()) {
            cam = ow->getSpaceCamera()->getCamera();
        }
        if (!cam) {
            continue;
        }
        if (isNear) {
            cam->setNearClipDistance(v);
        } else {
            cam->setFarClipDistance(v);
        }
    }
}

void applyFsaaByRebuildingViewports()
{
    if (auto* mainWin = Manager::getSingleton()->getMainWindow()) {
        mainWin->rebuildAllOgreViewports();
    }
}

bool tryApplyViewportSetting(const QString& key, const QVariant& value)
{
    if (key == ViewportSettingsKeys::gridVisible()) {
        applyGridVisibleFromSettings(value);
        return true;
    }
    if (key == ViewportSettingsKeys::cameraSpeed()) {
        applyCameraSpeedToAllViewports(value.toReal());
        return true;
    }
    if (key == ViewportSettingsKeys::nearClip() || key == ViewportSettingsKeys::farClip()) {
        applyClipPlaneToAllViewports(key, value);
        return true;
    }
    if (key == ViewportSettingsKeys::fsaaSamples()) {
        applyFsaaByRebuildingViewports();
        return true;
    }
    return false;
}

void applyEditorThemeFromString(const QString& themeLower)
{
    if (themeLower == QStringLiteral("dark")) {
        QPalette dark;
        dark.setColor(QPalette::Window, QColor(53, 53, 53));
        dark.setColor(QPalette::WindowText, Qt::white);
        dark.setColor(QPalette::Base, QColor(35, 35, 35));
        dark.setColor(QPalette::AlternateBase, QColor(53, 53, 53));
        dark.setColor(QPalette::ToolTipBase, QColor(25, 25, 25));
        dark.setColor(QPalette::ToolTipText, Qt::white);
        dark.setColor(QPalette::Text, Qt::white);
        dark.setColor(QPalette::Button, QColor(53, 53, 53));
        dark.setColor(QPalette::ButtonText, Qt::white);
        dark.setColor(QPalette::Link, QColor(42, 130, 218));
        dark.setColor(QPalette::Highlight, QColor(42, 130, 218));
        dark.setColor(QPalette::HighlightedText, Qt::black);
        QApplication::setPalette(dark);
        return;
    }
    if (themeLower == QStringLiteral("light")) {
        QApplication::setPalette(QColor(QStringLiteral("ghostwhite")));
    }
    // "system" = use platform default (may require restart)
}
} // namespace

// Generic QSettings accessors for Preferences dialog
QVariant PropertiesPanelController::getSetting(const QString& key, const QVariant& defaultValue) const
{
    QSettings settings;
    return settings.value(key, defaultValue);
}

void PropertiesPanelController::setSetting(const QString& key, const QVariant& value)
{
    QSettings settings;
    settings.setValue(key, value);
    SentryReporter::addBreadcrumb("ui.action",
        QString("Preference changed: %1").arg(key));

    if (tryApplyViewportSetting(key, value)) {
        return;
    }
    if (key == AppSettingsKeys::sentryEnabled() || key == AppSettingsKeys::telemetryEnabled()) {
        SentryReporter::setEnabled(value.toBool());
        return;
    }
    if (key == AppSettingsKeys::appearanceTheme() || key == AppSettingsKeys::palette()) {
        applyEditorThemeFromString(value.toString().toLower());
    }
}
