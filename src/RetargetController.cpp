/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "RetargetController.h"

#include "GamificationManager.h"
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "commands/RetargetAnimationCommand.h"

#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <QVariantMap>

#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreEntity.h>
#include <OgreSceneNode.h>
#include <OgreSkeletonInstance.h>

#include <set>

RetargetController* RetargetController::s_instance = nullptr;

namespace {
constexpr const char* kPreviewAnim = "__qtme_retarget_preview";

QWidget* dialogParent()
{
    QApplication::processEvents();
    QWidget* p = QApplication::activeWindow();
    if (p) { p->raise(); p->activateWindow(); }
    return p;
}
}  // namespace

RetargetController* RetargetController::instance()
{
    if (!s_instance) s_instance = new RetargetController();
    return s_instance;
}

RetargetController* RetargetController::qmlInstance(QQmlEngine*, QJSEngine*)
{
    // C++ owns the process-wide singleton; without this a QML engine being
    // destroyed would delete it and leave instance()/kill() dangling.
    RetargetController* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void RetargetController::kill()
{
    if (!s_instance) return;
    s_instance->stopPreview();
    delete s_instance;
    s_instance = nullptr;
}

RetargetController::RetargetController(QObject* parent)
    : QObject(parent)
{
    m_previewTimer.setInterval(33);
    connect(&m_previewTimer, &QTimer::timeout, this, &RetargetController::tickPreview);
    if (auto* mgr = Manager::getSingletonPtr()) {
        connect(mgr, &Manager::entityCreated, this, [this](Ogre::Entity* const&) { refresh(); });
        connect(mgr, &Manager::sceneNodeDestroyed, this, [this](Ogre::SceneNode* const&) {
            QTimer::singleShot(0, this, &RetargetController::refresh);
        });
        connect(mgr, &Manager::sceneClearing, this, [this]() { stopPreview(); });
    }
    refresh();
}

RetargetController::~RetargetController() = default;

// ---------------------------------------------------------------------------
// lookup / properties
// ---------------------------------------------------------------------------

Ogre::Entity* RetargetController::entityByName(const QString& name) const
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr || name.isEmpty()) return nullptr;
    const std::string n = name.toStdString();
    for (Ogre::MovableObject* obj : mgr->getEntities())
        if (obj && obj->getMovableType() == "Entity" && obj->getName() == n)
            return static_cast<Ogre::Entity*>(obj);
    return nullptr;
}

void RetargetController::setStatus(const QString& s, bool ok)
{
    m_status = s;
    m_lastOk = ok;
    emit statusChanged();
}

void RetargetController::refresh()
{
    QStringList fresh;
    if (auto* mgr = Manager::getSingletonPtr())
        for (Ogre::MovableObject* obj : mgr->getEntities())
            if (obj && obj->getMovableType() == "Entity"
                && static_cast<Ogre::Entity*>(obj)->hasSkeleton())
                fresh << QString::fromStdString(obj->getName());
    const bool changed = fresh != m_entities;
    m_entities = fresh;
    bool selChanged = false;
    if (!m_source.isEmpty() && !m_entities.contains(m_source)) { m_source.clear(); selChanged = true; }
    if (!m_target.isEmpty() && !m_entities.contains(m_target)) { m_target.clear(); selChanged = true; }
    if (m_previewing && (m_source.isEmpty() || m_target.isEmpty())) stopPreview();
    if (changed) emit entitiesChanged();
    if (selChanged) emit selectionChanged();
}

void RetargetController::noteOpened()
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("Retarget Animation opened"));
    refresh();
    // Preselect: the selected skeletal entity is the most likely TARGET (the
    // character you are animating); the source defaults to any other one.
    if (m_target.isEmpty()) {
        if (auto* sel = SelectionSet::getSingleton()) {
            const auto ents = sel->getResolvedEntities();
            for (Ogre::Entity* e : ents)
                if (e && e->hasSkeleton()) { setTargetEntity(QString::fromStdString(e->getName())); break; }
        }
    }
    if (m_source.isEmpty())
        for (const QString& n : m_entities)
            if (n != m_target) { setSourceEntity(n); break; }
}

QStringList RetargetController::sourceAnimations() const
{
    QStringList out;
    Ogre::Entity* e = entityByName(m_source);
    if (!e || !e->hasSkeleton()) return out;
    Ogre::SkeletonInstance* skel = e->getSkeleton();
    for (unsigned short i = 0; i < skel->getNumAnimations(); ++i) {
        const std::string n = skel->getAnimation(i)->getName();
        if (n != kPreviewAnim) out << QString::fromStdString(n);
    }
    return out;
}

QStringList RetargetController::targetBones() const
{
    QStringList out{QString()};   // "" = unmapped
    Ogre::Entity* e = entityByName(m_target);
    if (!e || !e->hasSkeleton()) return out;
    for (const auto& n : Retarget::boneNames(e->getSkeleton()))
        out << QString::fromStdString(n);
    return out;
}

QVariantList RetargetController::mappingRows() const
{
    QVariantList rows;
    Ogre::Entity* e = entityByName(m_source);
    if (!e || !e->hasSkeleton()) return rows;
    const Retarget::RigDesc rig = Retarget::describeSkeleton(e->getSkeleton());
    std::vector<int> depth(size_t(rig.size()), 0);
    for (int i = 0; i < rig.size(); ++i)
        depth[size_t(i)] = rig.parent[size_t(i)] >= 0 ? depth[size_t(rig.parent[size_t(i)])] + 1 : 0;
    for (int i = 0; i < rig.size(); ++i) {
        QVariantMap row;
        row[QStringLiteral("source")] = QString::fromStdString(rig.names[size_t(i)]);
        row[QStringLiteral("depth")] = depth[size_t(i)];
        row[QStringLiteral("target")] = QString::fromStdString(m_map.targetFor(rig.names[size_t(i)]));
        rows << row;
    }
    return rows;
}

void RetargetController::setSourceEntity(const QString& name)
{
    if (name == m_source) return;
    stopPreview();
    m_source = name;
    const QStringList anims = sourceAnimations();
    m_sourceAnim = anims.isEmpty() ? QString() : anims.first();
    emit selectionChanged();
    if (!m_source.isEmpty() && !m_target.isEmpty()) autoMap();
    else emit mappingChanged();
}

void RetargetController::setTargetEntity(const QString& name)
{
    if (name == m_target) return;
    stopPreview();
    m_target = name;
    emit selectionChanged();
    if (!m_source.isEmpty() && !m_target.isEmpty()) autoMap();
    else emit mappingChanged();
}

void RetargetController::setSourceAnimation(const QString& name)
{
    if (name == m_sourceAnim) return;
    // A different source clip changes BOTH sides of the preview: restart it
    // (stop restores the source's states, start enables the new clip).
    // Rebuilding only the target left the source frozen on the old clip.
    const bool wasPreviewing = m_previewing;
    if (wasPreviewing) stopPreview();
    m_sourceAnim = name;
    emit selectionChanged();
    if (wasPreviewing) startPreview();
}

void RetargetController::setTranslationMode(const QString& id)
{
    Retarget::TranslationMode m;
    if (!Retarget::translationModeFromId(id, &m) || m == m_opts.translation) return;
    m_opts.translation = m;
    emit optionsChanged();
    if (m_previewing) rebuildPreview();
}

void RetargetController::setAlignDirections(bool on)
{
    if (on == m_opts.alignDirections) return;
    m_opts.alignDirections = on;
    emit optionsChanged();
    if (m_previewing) rebuildPreview();
}

void RetargetController::setSourceRest(const QString& id)
{
    Retarget::SourceRest r;
    if (!Retarget::sourceRestFromId(id, &r) || r == m_opts.sourceRest) return;
    m_opts.sourceRest = r;
    emit optionsChanged();
    if (m_previewing) rebuildPreview();
}

void RetargetController::setNewAnimationName(const QString& name)
{
    if (name == m_newName) return;
    m_newName = name;
    emit optionsChanged();
}

// ---------------------------------------------------------------------------
// mapping
// ---------------------------------------------------------------------------

void RetargetController::autoMap()
{
    Ogre::Entity* s = entityByName(m_source);
    Ogre::Entity* t = entityByName(m_target);
    if (!s || !t || !s->hasSkeleton() || !t->hasSkeleton()) {
        setStatus(QStringLiteral("Pick a source and a target with skeletons first."), false);
        return;
    }
    const auto rep = Retarget::autoMap(Retarget::boneNames(s->getSkeleton()),
                                       Retarget::boneNames(t->getSkeleton()));
    m_map = rep.map;
    emit mappingChanged();
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.retarget.automap"),
        QStringLiteral("auto-map %1 pairs (name %2, role %3, fuzzy %4), %5 source unmapped")
            .arg(m_map.pairs.size()).arg(rep.byExactName).arg(rep.byRole).arg(rep.byFuzzy)
            .arg(rep.unmappedSource.size()));
    setStatus(QStringLiteral("Auto-mapped %1 bones (%2 source bones left unmapped).")
                  .arg(m_map.pairs.size()).arg(rep.unmappedSource.size()), true);
    if (m_previewing) rebuildPreview();
}

void RetargetController::setPairTarget(const QString& sourceBone, const QString& targetBone)
{
    m_map.setPair(sourceBone.toStdString(), targetBone.toStdString());
    emit mappingChanged();
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.retarget.map_edit"),
        QStringLiteral("%1 -> %2").arg(sourceBone, targetBone.isEmpty() ? QStringLiteral("(none)") : targetBone));
    if (m_previewing) rebuildPreview();
}

void RetargetController::clearMapping()
{
    m_map.pairs.clear();
    emit mappingChanged();
    if (m_previewing) stopPreview();
}

bool RetargetController::loadBundledMap(const QString& name)
{
    Retarget::BoneMap m;
    if (!Retarget::bundledBoneMap(name, &m)) {
        setStatus(QStringLiteral("Unknown bundled map '%1'.").arg(name), false);
        return false;
    }
    Ogre::Entity* s = entityByName(m_source);
    Ogre::Entity* t = entityByName(m_target);
    if (s && t && s->hasSkeleton() && t->hasSkeleton())
        m = Retarget::resolveMap(m, Retarget::boneNames(s->getSkeleton()),
                                 Retarget::boneNames(t->getSkeleton()));
    m_map = m;
    emit mappingChanged();
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.retarget.bonemap_load"),
        QStringLiteral("bundled %1 (%2 pairs)").arg(name).arg(m_map.pairs.size()));
    setStatus(QStringLiteral("Loaded '%1': %2 pairs match these skeletons.").arg(name).arg(m_map.pairs.size()),
              !m_map.pairs.empty());
    if (m_previewing) rebuildPreview();
    return true;
}

bool RetargetController::loadBoneMapFrom(const QString& path)
{
    Retarget::BoneMap m;
    QString err;
    if (!Retarget::BoneMap::load(path, &m, &err)) { setStatus(err, false); return false; }
    QStringList unresolved;
    Ogre::Entity* s = entityByName(m_source);
    Ogre::Entity* t = entityByName(m_target);
    if (s && t && s->hasSkeleton() && t->hasSkeleton())
        m = Retarget::resolveMap(m, Retarget::boneNames(s->getSkeleton()),
                                 Retarget::boneNames(t->getSkeleton()), &unresolved);
    m_map = m;
    emit mappingChanged();
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.retarget.bonemap_load"),
        QStringLiteral("file %1 (%2 pairs, %3 unresolved)").arg(QFileInfo(path).fileName())
            .arg(m_map.pairs.size()).arg(unresolved.size()));
    setStatus(unresolved.isEmpty()
                  ? QStringLiteral("Loaded %1 pairs from %2.").arg(m_map.pairs.size()).arg(QFileInfo(path).fileName())
                  : QStringLiteral("Loaded %1 pairs; %2 pairs name bones these skeletons don't have.")
                        .arg(m_map.pairs.size()).arg(unresolved.size()),
              true);
    if (m_previewing) rebuildPreview();
    return true;
}

bool RetargetController::saveBoneMapTo(const QString& path)
{
    QString err;
    Retarget::BoneMap m = m_map;
    if (m.name.isEmpty() || m.name == QLatin1String("auto"))
        m.name = QFileInfo(path).completeBaseName();
    if (!m.save(path, &err)) { setStatus(err, false); return false; }
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.retarget.bonemap_save"),
        QStringLiteral("%1 (%2 pairs)").arg(QFileInfo(path).fileName()).arg(m.pairs.size()));
    setStatus(QStringLiteral("Saved %1 pairs to %2.").arg(m.pairs.size()).arg(QFileInfo(path).fileName()), true);
    return true;
}

bool RetargetController::loadBoneMapFile()
{
    const QString path = QFileDialog::getOpenFileName(
        dialogParent(), QStringLiteral("Load bone map"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        QStringLiteral("Bone maps (*.bonemap *.json)"), nullptr, QFileDialog::DontUseNativeDialog);
    return !path.isEmpty() && loadBoneMapFrom(path);
}

bool RetargetController::saveBoneMapFile()
{
    QString path = QFileDialog::getSaveFileName(
        dialogParent(), QStringLiteral("Save bone map"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
            + QStringLiteral("/mapping.bonemap"),
        QStringLiteral("Bone maps (*.bonemap)"), nullptr, QFileDialog::DontUseNativeDialog);
    if (path.isEmpty()) return false;
    if (!path.endsWith(QLatin1String(".bonemap"))) path += QStringLiteral(".bonemap");
    return saveBoneMapTo(path);
}

bool RetargetController::importSourceFile()
{
    const QString path = QFileDialog::getOpenFileName(
        dialogParent(), QStringLiteral("Import animation source"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        QStringLiteral("Animated meshes (*.fbx *.glb *.gltf *.dae *.mesh *.bvh);;All files (*)"),
        nullptr, QFileDialog::DontUseNativeDialog);
    if (path.isEmpty()) return false;
    const QSet<QString> before(m_entities.begin(), m_entities.end());
    SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
        QStringLiteral("Retarget source import %1").arg(QFileInfo(path).fileName()));
    MeshImporterExporter::importer({path});
    refresh();
    for (const QString& n : m_entities)
        if (!before.contains(n)) {
            setSourceEntity(n);
            setStatus(QStringLiteral("Imported %1 as the source.").arg(QFileInfo(path).fileName()), true);
            return true;
        }
    setStatus(QStringLiteral("%1 has no skeleton to retarget from.").arg(QFileInfo(path).fileName()), false);
    return false;
}

// ---------------------------------------------------------------------------
// preview
// ---------------------------------------------------------------------------

std::string RetargetController::defaultNewName() const
{
    const QString base = m_newName.trimmed().isEmpty()
        ? m_sourceAnim + QStringLiteral("_retargeted") : m_newName.trimmed();
    return base.toStdString();
}

bool RetargetController::rebuildPreview()
{
    Ogre::Entity* s = entityByName(m_source);
    Ogre::Entity* t = entityByName(m_target);
    if (!s || !t || !s->hasSkeleton() || !t->hasSkeleton() || m_sourceAnim.isEmpty()) return false;
    Ogre::SkeletonInstance* ts = t->getSkeleton();
    if (ts->hasAnimation(kPreviewAnim)) {
        ts->removeAnimation(kPreviewAnim);
        // drop the state too: refreshAvailableAnimationState never removes
        if (auto* st = t->getAllAnimationStates())
            if (st->hasAnimationState(kPreviewAnim)) st->removeAnimationState(kPreviewAnim);
        t->refreshAvailableAnimationState();
    }
    const Retarget::Result r = Retarget::retarget(s->getSkeleton(), m_sourceAnim.toStdString(),
                                                  ts, kPreviewAnim, m_map, m_opts);
    if (!r.ok) { setStatus(r.error, false); return false; }
    t->refreshAvailableAnimationState();
    auto* st = t->getAllAnimationStates()->getAnimationState(kPreviewAnim);
    st->setEnabled(true);
    st->setLoop(true);
    st->setWeight(1.0f);
    if (auto* ss = s->getAllAnimationStates())
        if (ss->hasAnimationState(m_sourceAnim.toStdString())) {
            auto* a = ss->getAnimationState(m_sourceAnim.toStdString());
            st->setTimePosition(a->getTimePosition());
        }
    m_previewAnim = kPreviewAnim;
    return true;
}

bool RetargetController::startPreview()
{
    if (m_previewing) return true;
    Ogre::Entity* s = entityByName(m_source);
    Ogre::Entity* t = entityByName(m_target);
    if (!s || !t) { setStatus(QStringLiteral("Pick a source and a target first."), false); return false; }
    if (m_map.pairs.empty()) { setStatus(QStringLiteral("The bone map is empty — auto-map first."), false); return false; }

    // remember every state's flags so stopping restores the user's setup
    auto save = [](Ogre::Entity* e, std::map<std::string, SavedState>& out) {
        out.clear();
        if (auto* set = e->getAllAnimationStates())
            for (const auto& kv : set->getAnimationStates())
                out[kv.first] = {kv.second->getEnabled(), kv.second->getLoop(), kv.second->getTimePosition()};
    };
    save(s, m_savedSource);
    save(t, m_savedTarget);
    for (Ogre::Entity* e : {s, t})
        if (auto* set = e->getAllAnimationStates())
            for (const auto& kv : set->getAnimationStates()) kv.second->setEnabled(false);

    if (!rebuildPreview()) {
        for (auto [e, saved] : {std::pair{s, &m_savedSource}, std::pair{t, &m_savedTarget}})
            if (auto* set = e->getAllAnimationStates())
                for (const auto& [n, st] : *saved)
                    if (set->hasAnimationState(n)) set->getAnimationState(n)->setEnabled(st.enabled);
        return false;
    }
    auto* srcState = s->getAllAnimationStates()->getAnimationState(m_sourceAnim.toStdString());
    srcState->setEnabled(true);
    srcState->setLoop(true);
    srcState->setTimePosition(0.0f);

    // side by side: if the two overlap, slide the target clear of the source
    m_targetMoved = false;
    if (s != t && s->getParentSceneNode() && t->getParentSceneNode()) {
        const Ogre::AxisAlignedBox bs = s->getWorldBoundingBox(true);
        const Ogre::AxisAlignedBox bt = t->getWorldBoundingBox(true);
        if (bs.intersects(bt)) {
            Ogre::SceneNode* tn = t->getParentSceneNode();
            m_targetOrigPos = tn->getPosition();
            const float shift = bs.getMaximum().x - bt.getMinimum().x + 0.15f * bs.getSize().x;
            tn->translate(Ogre::Vector3(shift, 0, 0), Ogre::Node::TS_WORLD);
            m_targetMoved = true;
        }
    }

    m_previewing = true;
    m_previewTimer.start();
    emit previewingChanged();
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.retarget.preview"),
        QStringLiteral("preview %1 -> %2 (%3 pairs)").arg(m_source, m_target).arg(m_map.pairs.size()));
    setStatus(QStringLiteral("Previewing: source on the left, retargeted target on the right."), true);
    return true;
}

void RetargetController::tickPreview()
{
    Ogre::Entity* s = entityByName(m_source);
    Ogre::Entity* t = entityByName(m_target);
    if (!s || !t) { stopPreview(); return; }
    const float dt = float(m_previewTimer.interval()) / 1000.0f;
    if (auto* set = s->getAllAnimationStates())
        if (set->hasAnimationState(m_sourceAnim.toStdString()))
            set->getAnimationState(m_sourceAnim.toStdString())->addTime(dt);
    if (auto* set = t->getAllAnimationStates())
        if (set->hasAnimationState(kPreviewAnim))
            set->getAnimationState(kPreviewAnim)->addTime(dt);
}

void RetargetController::stopPreview()
{
    if (!m_previewing) return;
    m_previewTimer.stop();
    m_previewing = false;
    Ogre::Entity* s = entityByName(m_source);
    Ogre::Entity* t = entityByName(m_target);
    if (t && t->hasSkeleton() && t->getSkeleton()->hasAnimation(kPreviewAnim)) {
        t->getSkeleton()->removeAnimation(kPreviewAnim);
        if (auto* set = t->getAllAnimationStates())
            if (set->hasAnimationState(kPreviewAnim)) set->removeAnimationState(kPreviewAnim);
        t->refreshAvailableAnimationState();
    }
    auto restore = [](Ogre::Entity* e, const std::map<std::string, SavedState>& saved) {
        if (!e) return;
        if (auto* set = e->getAllAnimationStates())
            for (const auto& [n, st] : saved)
                if (set->hasAnimationState(n)) {
                    auto* a = set->getAnimationState(n);
                    a->setEnabled(st.enabled);
                    a->setLoop(st.loop);
                    a->setTimePosition(st.time);
                }
    };
    restore(s, m_savedSource);
    restore(t, m_savedTarget);
    if (t && m_targetMoved && t->getParentSceneNode())
        t->getParentSceneNode()->setPosition(m_targetOrigPos);
    m_targetMoved = false;
    m_previewAnim.clear();
    emit previewingChanged();
}

// ---------------------------------------------------------------------------
// apply
// ---------------------------------------------------------------------------

bool RetargetController::apply()
{
    stopPreview();
    Ogre::Entity* s = entityByName(m_source);
    Ogre::Entity* t = entityByName(m_target);
    if (!s || !t || !s->hasSkeleton() || !t->hasSkeleton()) {
        setStatus(QStringLiteral("Pick a source and a target with skeletons first."), false);
        return false;
    }
    if (m_sourceAnim.isEmpty()) { setStatus(QStringLiteral("Pick the source clip."), false); return false; }
    if (m_map.pairs.empty()) { setStatus(QStringLiteral("The bone map is empty — auto-map first."), false); return false; }

    const std::string name = Retarget::uniqueAnimationName(t->getSkeleton(), defaultNewName());
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.retarget.apply"),
        QStringLiteral("%1:%2 -> %3 as '%4' (%5 pairs, translation=%6, align=%7, rest=%8)")
            .arg(m_source, m_sourceAnim, m_target, QString::fromStdString(name))
            .arg(m_map.pairs.size()).arg(translationMode()).arg(m_opts.alignDirections).arg(sourceRest()));
    const Retarget::Result r = Retarget::retarget(s->getSkeleton(), m_sourceAnim.toStdString(),
                                                  t->getSkeleton(), name, m_map, m_opts);
    if (!r.ok) {
        SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.retarget.error"), r.error);
        setStatus(r.error, false);
        return false;
    }
    t->refreshAvailableAnimationState();
    if (auto* um = UndoManager::getSingleton())
        um->push(new RetargetAnimationCommand(t->getName(), name, /*alreadyApplied=*/true));
    GamificationManager::noteFeature(QStringLiteral("animation_blend"));
    const QString qn = QString::fromStdString(name);
    setStatus(QStringLiteral("Created '%1' on %2: %3 frames, %4 bones driven%5.")
                  .arg(qn, m_target).arg(r.frames).arg(r.report.mappedBones)
                  .arg(r.report.globalAlignment ? QString() : QStringLiteral(" (no humanoid frame — axes used as-is)")),
              true);
    emit applied(m_target, qn);
    return true;
}
