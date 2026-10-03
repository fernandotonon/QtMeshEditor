// #525 — ConstraintManager: per-frame evaluation on live nodes and bones,
// stack order, mute / remove restore, bake, undo, sidecar and the panel QML.

#include <gtest/gtest.h>

#include "AnimConstraints.h"
#include "AnimationControlController.h"
#include "ConstraintManager.h"
#include "Manager.h"
#include "NodeAnimationManager.h"
#include "PropertiesPanelController.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "ThemeManager.h"
#include "UndoManager.h"
#include "commands/ConstraintCommands.h"

#include <QQmlComponent>
#include <QQmlEngine>
#include <QTemporaryDir>

#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreAnimationTrack.h>
#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreKeyFrame.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSkeletonInstance.h>
#include <OgreSkeletonManager.h>

using namespace AnimCon;

namespace {

ConstraintManager* mgr() { return ConstraintManager::instance(); }

Ogre::SceneNode* makeNode(const char* name, const Ogre::Vector3& pos)
{
    Ogre::SceneNode* n = Manager::getSingleton()->addSceneNode(QString::fromLatin1(name));
    n->setPosition(pos);
    return n;
}

QString nodeRef(Ogre::SceneNode* n) { return QStringLiteral("node:") + QString::fromStdString(n->getName()); }

Constraint make(Type t, const QString& owner, const QString& target)
{
    Constraint c;
    c.type = t;
    EXPECT_TRUE(parseRef(owner, &c.owner));
    EXPECT_TRUE(parseRef(target, &c.target));
    return c;
}

// Root (origin) → Mid (0,1,0.2) → End (0,1,0) relative: a slightly bent
// 2-bone chain, with a 1 s "Idle" clip that keys Root at identity.
Ogre::Entity* makeChainEntity(const std::string& name)
{
    auto skel = Ogre::SkeletonManager::getSingleton().create(name + "_skel",
                                                             Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* root = skel->createBone("Root", 0);
    auto* mid = skel->createBone("Mid", 1);
    auto* end = skel->createBone("End", 2);
    mid->setPosition(0, 1, 0.2f);
    end->setPosition(0, 1, 0);
    root->addChild(mid);
    mid->addChild(end);
    skel->setBindingPose();
    auto* anim = skel->createAnimation("Idle", 1.0f);
    auto* tr = anim->createNodeTrack(0, root);
    tr->createNodeKeyFrame(0.0f);
    tr->createNodeKeyFrame(1.0f);

    auto mesh = Ogre::MeshManager::getSingleton().createManual(name + "_mesh",
                                                               Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* sub = mesh->createSubMesh();
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    float verts[] = {0, 0, 0, 0, 2, 0, 0.1f, 1, 0};
    vbuf->writeData(0, sizeof(verts), verts);
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    mesh->sharedVertexData->vertexCount = 3;
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(Ogre::HardwareIndexBuffer::IT_16BIT, 3,
                                                                              Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint16_t idx[] = {0, 1, 2};
    ibuf->writeData(0, sizeof(idx), idx);
    sub->useSharedVertices = true;
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 3;
    for (unsigned short v = 0; v < 3; ++v) {
        Ogre::VertexBoneAssignment vba;
        vba.vertexIndex = v;
        vba.boneIndex = v;
        vba.weight = 1.0f;
        mesh->addBoneAssignment(vba);
    }
    mesh->_notifySkeleton(skel);
    mesh->_setBounds(Ogre::AxisAlignedBox(-1, -1, -1, 2, 2, 2));
    mesh->_setBoundingSphereRadius(3.0);
    mesh->load();
    auto* node = Manager::getSingleton()->addSceneNode(QString::fromStdString(name));
    auto* e = Manager::getSingleton()->getSceneMgr()->createEntity(name, mesh);
    node->attachObject(e);
    return e;
}

Ogre::Vector3 boneWorld(Ogre::Entity* e, const char* bone)
{
    Ogre::SceneNode* n = e->getParentSceneNode();
    return n->_getDerivedPosition()
         + n->_getDerivedOrientation() * (n->_getDerivedScale() * e->getSkeleton()->getBone(bone)->_getDerivedPosition());
}

bool sameDir(const Ogre::Vector3& a, const Ogre::Vector3& b) { return a.normalisedCopy().dotProduct(b.normalisedCopy()) > 0.9999f; }

} // namespace

// ---------------------------------------------------------------------------
// No scene needed
// ---------------------------------------------------------------------------

TEST(ConstraintManagerStandalone, RejectsMissingObjectsWithoutTouchingState)
{
    mgr()->clear();
    const auto r = mgr()->add(make(Type::LookAt, QStringLiteral("node:NoSuchOwner"), QStringLiteral("node:NoSuchTarget")), false);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("NoSuchOwner"))) << r.error.toStdString();
    EXPECT_EQ(mgr()->count(), 0);
    EXPECT_FALSE(mgr()->lastOk());
    EXPECT_FALSE(mgr()->remove(QStringLiteral("nope"), false).ok);
    EXPECT_FALSE(mgr()->move(QStringLiteral("nope"), -1, false).ok);
    EXPECT_FALSE(mgr()->bake({}, false).ok) << "nothing to bake";
}

TEST(ConstraintManagerStandalone, DocCommandSkipsItsFirstRedo)
{
    mgr()->clear();
    const QJsonObject empty = mgr()->document();
    ConstraintDocCommand cmd(QStringLiteral("x"), empty, empty);
    cmd.redo();
    cmd.undo();
    cmd.redo();
    EXPECT_EQ(mgr()->count(), 0);
}

TEST(ConstraintManagerStandalone, SidecarPathSitsBesideTheAsset)
{
    EXPECT_TRUE(ConstraintManager::sidecarPath(QStringLiteral("/a/b/hero.glb")).endsWith(QStringLiteral("/a/b/hero.constraints.json")));
}

// ---------------------------------------------------------------------------
// Live scene
// ---------------------------------------------------------------------------

class ConstraintSceneTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        mgr()->clear();
        // Other suites leave undone commands on the shared stack; a push
        // would truncate that redo tail and skew count-based assertions.
        UndoManager::getSingleton()->clear();
    }
    void TearDown() override
    {
        mgr()->clear();
        UndoManager::getSingleton()->clear();
        if (auto* sel = SelectionSet::getSingletonPtr()) sel->clear();
        if (auto* m = Manager::getSingletonPtr())
            if (auto* scene = m->getSceneMgr()) {
                for (const QString& c : NodeAnimationManager::instance()->listClips())
                    NodeAnimationManager::instance()->deleteClip(c);
                try { scene->destroyAllEntities(); } catch (...) {}
                try { scene->getRootSceneNode()->removeAndDestroyAllChildren(); } catch (...) {}
            }
    }
};

TEST_F(ConstraintSceneTest, LookAtAimsTheNodeAndRemovingRestoresIt)
{
    Ogre::SceneNode* owner = makeNode("CON_LookOwner", Ogre::Vector3::ZERO);
    Ogre::SceneNode* target = makeNode("CON_LookTarget", Ogre::Vector3(5, 0, 0));
    const auto r = mgr()->add(make(Type::LookAt, nodeRef(owner), nodeRef(target)), false);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(mgr()->activeCount(), 1);
    mgr()->evaluate();
    EXPECT_TRUE(sameDir(owner->_getDerivedOrientation() * Ogre::Vector3::UNIT_Z, Ogre::Vector3::UNIT_X))
        << "the +Z aim axis points at the target";

    target->setPosition(0, 0, -5);
    mgr()->evaluate();
    EXPECT_TRUE(sameDir(owner->_getDerivedOrientation() * Ogre::Vector3::UNIT_Z, Ogre::Vector3::NEGATIVE_UNIT_Z))
        << "re-evaluated every frame, from the base (no accumulation)";

    ASSERT_TRUE(mgr()->remove(r.id, false).ok);
    EXPECT_LT(std::abs(std::abs(owner->getOrientation().Dot(Ogre::Quaternion::IDENTITY)) - 1.0f), 1e-5f)
        << "removing the constraint restores the node's own rotation";
}

TEST_F(ConstraintSceneTest, InfluenceBlendsAndNeverCreeps)
{
    Ogre::SceneNode* owner = makeNode("CON_InfOwner", Ogre::Vector3::ZERO);
    Ogre::SceneNode* target = makeNode("CON_InfTarget", Ogre::Vector3(4, 0, 0));
    Constraint c = make(Type::CopyPosition, nodeRef(owner), nodeRef(target));
    c.influence = 0.5;
    ASSERT_TRUE(mgr()->add(c, false).ok);
    for (int i = 0; i < 5; ++i) mgr()->evaluate();
    EXPECT_NEAR(owner->getPosition().x, 2.0f, 1e-5f) << "half way, however many frames run";
}

TEST_F(ConstraintSceneTest, CopyPositionHonoursTheAxisMask)
{
    Ogre::SceneNode* owner = makeNode("CON_MaskOwner", Ogre::Vector3(1, 1, 1));
    Ogre::SceneNode* target = makeNode("CON_MaskTarget", Ogre::Vector3(7, 8, 9));
    Constraint c = make(Type::CopyPosition, nodeRef(owner), nodeRef(target));
    c.useY = false;
    ASSERT_TRUE(mgr()->add(c, false).ok);
    mgr()->evaluate();
    EXPECT_LT((owner->getPosition() - Ogre::Vector3(7, 1, 9)).length(), 1e-5f);
}

TEST_F(ConstraintSceneTest, ParentOfKeepsTheOwnerWhereItWasAndFollowsTheTarget)
{
    Ogre::SceneNode* owner = makeNode("CON_ChildOwner", Ogre::Vector3(2, 0, 0));
    Ogre::SceneNode* target = makeNode("CON_ChildTarget", Ogre::Vector3(0, 0, 0));
    ASSERT_TRUE(mgr()->add(make(Type::ParentOf, nodeRef(owner), nodeRef(target)), false).ok);
    mgr()->evaluate();
    EXPECT_LT((owner->getPosition() - Ogre::Vector3(2, 0, 0)).length(), 1e-5f) << "offset captured at add";
    target->setPosition(0, 3, 0);
    target->setOrientation(Ogre::Quaternion(Ogre::Degree(90), Ogre::Vector3::UNIT_Y));
    mgr()->evaluate();
    // (2,0,0) rotated 90° about Y = (0,0,-2), then lifted by the target.
    EXPECT_LT((owner->getPosition() - Ogre::Vector3(0, 3, -2)).length(), 1e-4f);
}

TEST_F(ConstraintSceneTest, LimitRotationClampsTheLocalAngle)
{
    Ogre::SceneNode* owner = makeNode("CON_Limit", Ogre::Vector3::ZERO);
    owner->setOrientation(Ogre::Quaternion(Ogre::Degree(80), Ogre::Vector3::UNIT_X));
    Constraint c;
    c.type = Type::LimitRotation;
    ASSERT_TRUE(parseRef(nodeRef(owner), &c.owner));
    ASSERT_TRUE(mgr()->add(c, false).ok);
    mgr()->evaluate();
    Ogre::Radian ang;
    Ogre::Vector3 axis;
    owner->getOrientation().ToAngleAxis(ang, axis);
    EXPECT_NEAR(ang.valueDegrees(), 45.0f, 0.1f);
}

TEST_F(ConstraintSceneTest, TopOfTheStackWinsAndReorderingFlipsIt)
{
    Ogre::SceneNode* owner = makeNode("CON_StackOwner", Ogre::Vector3::ZERO);
    Ogre::SceneNode* a = makeNode("CON_StackA", Ogre::Vector3(1, 0, 0));
    Ogre::SceneNode* b = makeNode("CON_StackB", Ogre::Vector3(0, 0, 9));
    const auto ra = mgr()->add(make(Type::CopyPosition, nodeRef(owner), nodeRef(a)), false);
    const auto rb = mgr()->add(make(Type::CopyPosition, nodeRef(owner), nodeRef(b)), false);
    ASSERT_TRUE(ra.ok && rb.ok);
    const QVariantList rows = mgr()->rowsFor(nodeRef(owner));
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[0].toMap().value(QStringLiteral("id")).toString(), rb.id) << "a new constraint goes on top";
    mgr()->evaluate();
    EXPECT_LT((owner->getPosition() - b->getPosition()).length(), 1e-5f) << "top-most wins";

    ASSERT_TRUE(mgr()->move(rb.id, +1, false).ok);
    mgr()->evaluate();
    EXPECT_LT((owner->getPosition() - a->getPosition()).length(), 1e-5f);
    EXPECT_FALSE(mgr()->move(rb.id, +1, false).ok) << "already at the bottom";
}

TEST_F(ConstraintSceneTest, MutingHandsTheNodeBackAndEveryEditIsOneUndoStep)
{
    Ogre::SceneNode* owner = makeNode("CON_UndoOwner", Ogre::Vector3(1, 2, 3));
    Ogre::SceneNode* target = makeNode("CON_UndoTarget", Ogre::Vector3(9, 9, 9));
    UndoManager* um = UndoManager::getSingleton();
    const int before = um->stack()->index();
    const auto r = mgr()->add(make(Type::CopyPosition, nodeRef(owner), nodeRef(target)));
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(um->stack()->index(), before + 1);
    mgr()->evaluate();
    ASSERT_LT((owner->getPosition() - target->getPosition()).length(), 1e-5f);

    const bool off = false;
    ASSERT_TRUE(mgr()->setParams(r.id, {}, &off).ok);
    EXPECT_EQ(mgr()->activeCount(), 0);
    EXPECT_LT((owner->getPosition() - Ogre::Vector3(1, 2, 3)).length(), 1e-5f) << "muting restores the base";

    ASSERT_TRUE(mgr()->setParams(r.id, {{QStringLiteral("influence"), QStringLiteral("0.25")}}, nullptr).ok);
    EXPECT_EQ(um->stack()->index(), before + 3);
    um->undo();
    EXPECT_DOUBLE_EQ(mgr()->find(r.id)->influence, 1.0);
    um->undo();
    EXPECT_TRUE(mgr()->find(r.id)->enabled);
    um->undo();
    EXPECT_EQ(mgr()->count(), 0);
    um->redo();
    EXPECT_EQ(mgr()->count(), 1);
}

TEST_F(ConstraintSceneTest, BadParamsAreRejectedAtomically)
{
    Ogre::SceneNode* owner = makeNode("CON_AtomOwner", Ogre::Vector3::ZERO);
    Ogre::SceneNode* target = makeNode("CON_AtomTarget", Ogre::Vector3(1, 0, 0));
    const auto r = mgr()->add(make(Type::LookAt, nodeRef(owner), nodeRef(target)), false);
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(mgr()->setParams(r.id, {{QStringLiteral("influence"), QStringLiteral("0.3")},
                                         {QStringLiteral("wobble"), QStringLiteral("1")}}, nullptr, false).ok);
    EXPECT_DOUBLE_EQ(mgr()->find(r.id)->influence, 1.0) << "nothing applied when any key fails";
    EXPECT_FALSE(mgr()->setParams(r.id, {{QStringLiteral("target"), QStringLiteral("node:Gone")}}, nullptr, false).ok);
    EXPECT_EQ(formatRef(mgr()->find(r.id)->target), nodeRef(target));
}

TEST_F(ConstraintSceneTest, ALaterConstraintSeesTheChildOfAnEarlierOwnerMove)
{
    // A follows T; B is A's child; X follows B. X must see B where A's
    // constraint put it THIS evaluation, not B's stale derived transform.
    Ogre::SceneNode* a = makeNode("CON_ChainA", Ogre::Vector3::ZERO);
    Ogre::SceneNode* b = a->createChildSceneNode("CON_ChainB", Ogre::Vector3(0, 1, 0));
    Ogre::SceneNode* t = makeNode("CON_ChainT", Ogre::Vector3(5, 0, 0));
    Ogre::SceneNode* x = makeNode("CON_ChainX", Ogre::Vector3::ZERO);
    a->_getDerivedPosition();
    b->_getDerivedPosition();
    ASSERT_TRUE(mgr()->add(make(Type::CopyPosition, nodeRef(a), nodeRef(t)), false).ok);
    ASSERT_TRUE(mgr()->add(make(Type::CopyPosition, nodeRef(x), QStringLiteral("node:CON_ChainB")), false).ok);
    mgr()->evaluate();
    EXPECT_LT((x->getPosition() - Ogre::Vector3(5, 1, 0)).length(), 1e-5f);
    (void)b;
}

TEST_F(ConstraintSceneTest, IkNeedsABoneChain)
{
    Ogre::Entity* e = createAnimatedTestEntity("CON_ShortChain");  // Root → Child only
    Ogre::SceneNode* target = makeNode("CON_ShortTarget", Ogre::Vector3(1, 1, 0));
    auto r = mgr()->add(make(Type::IK, QStringLiteral("bone:CON_ShortChain/Child"), nodeRef(target)), false);
    EXPECT_FALSE(r.ok) << "Child has no grandparent";
    r = mgr()->add(make(Type::IK, nodeRef(target), nodeRef(e->getParentSceneNode())), false);
    EXPECT_FALSE(r.ok) << "a node cannot own an IK";
}

TEST_F(ConstraintSceneTest, TwoBoneIkPutsTheEndBoneOnTheTarget)
{
    Ogre::Entity* e = makeChainEntity("CON_Arm");
    Ogre::SceneNode* target = makeNode("CON_ArmTarget", Ogre::Vector3(0.8f, 1.2f, 0));
    const auto r = mgr()->add(make(Type::IK, QStringLiteral("bone:CON_Arm/End"), nodeRef(target)), false);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    mgr()->evaluate();
    EXPECT_LT((boneWorld(e, "End") - target->getPosition()).length(), 1e-3f);
    const float upper = (boneWorld(e, "Mid") - boneWorld(e, "Root")).length();
    EXPECT_NEAR(upper, Ogre::Vector3(0, 1, 0.2f).length(), 1e-4f) << "bone lengths are preserved";

    // Muting hands sampling back to the clip next frame: the bones are not manual.
    EXPECT_FALSE(e->getSkeleton()->getBone("Mid")->isManuallyControlled());
}

TEST_F(ConstraintSceneTest, BakeWritesIkIntoTheClipMutesAndUndoes)
{
    Ogre::Entity* e = makeChainEntity("CON_Bake");
    Ogre::SceneNode* target = makeNode("CON_BakeTarget", Ogre::Vector3(-0.7f, 1.3f, 0.3f));
    const auto r = mgr()->add(make(Type::IK, QStringLiteral("bone:CON_Bake/End"), nodeRef(target)), false);
    ASSERT_TRUE(r.ok);
    Ogre::Animation* anim = e->getSkeleton()->getAnimation("Idle");
    const unsigned short midH = e->getSkeleton()->getBone("Mid")->getHandle();
    ASSERT_FALSE(anim->hasNodeTrack(midH));
    const int before = UndoManager::getSingleton()->stack()->index();

    ConstraintManager::BakeOptions o;
    o.clip = QStringLiteral("Idle");
    ASSERT_TRUE(mgr()->bake(o).ok) << mgr()->status().toStdString();
    EXPECT_EQ(UndoManager::getSingleton()->stack()->index(), before + 1);
    EXPECT_FALSE(mgr()->find(r.id)->enabled) << "the baked constraint is muted";
    ASSERT_TRUE(anim->hasNodeTrack(midH)) << "the IK's mid bone gets a track";
    EXPECT_EQ(anim->getNodeTrack(midH)->getNumKeyFrames(), 31u);

    // Play the clip WITHOUT the constraint: the keys reproduce the solve.
    Ogre::AnimationState* st = e->getAnimationState("Idle");
    st->setEnabled(true);
    st->setTimePosition(0.5f);
    e->getSkeleton()->setAnimationState(*e->getAllAnimationStates());
    EXPECT_LT((boneWorld(e, "End") - target->getPosition()).length(), 2e-3f);

    UndoManager::getSingleton()->undo();
    EXPECT_FALSE(anim->hasNodeTrack(midH)) << "undo removes the tracks the bake created";
    EXPECT_TRUE(mgr()->find(r.id)->enabled);
    UndoManager::getSingleton()->redo();
    EXPECT_TRUE(anim->hasNodeTrack(midH));
    EXPECT_FALSE(mgr()->find(r.id)->enabled);
}

TEST_F(ConstraintSceneTest, BakingANodeCreatesAConstraintsClip)
{
    Ogre::SceneNode* owner = makeNode("CON_NodeBake", Ogre::Vector3::ZERO);
    Ogre::SceneNode* target = makeNode("CON_NodeBakeTarget", Ogre::Vector3(3, 0, 0));
    ASSERT_TRUE(mgr()->add(make(Type::CopyPosition, nodeRef(owner), nodeRef(target)), false).ok);
    ASSERT_TRUE(mgr()->bake({}, false).ok) << mgr()->status().toStdString();
    EXPECT_TRUE(NodeAnimationManager::instance()->listClips().contains(QStringLiteral("Constraints")));
    EXPECT_TRUE(NodeAnimationManager::instance()->animatedNodes(QStringLiteral("Constraints"))
                    .contains(QString::fromStdString(owner->getName())));
    EXPECT_EQ(mgr()->activeCount(), 0);
}

TEST_F(ConstraintSceneTest, SidecarRoundTripRebindsRenamedObjects)
{
    Ogre::SceneNode* owner = makeNode("CON_SideOwner", Ogre::Vector3::ZERO);
    Ogre::SceneNode* target = makeNode("CON_SideTarget", Ogre::Vector3(1, 0, 0));
    Constraint c = make(Type::LookAt, nodeRef(owner), nodeRef(target));
    c.aimAxis = Axis::NegY;
    c.influence = 0.75;
    ASSERT_TRUE(mgr()->add(c, false).ok);
    QTemporaryDir dir;
    const QString asset = dir.filePath(QStringLiteral("scene.glb"));
    ASSERT_TRUE(mgr()->writeSidecar(asset, {}, QJsonObject{{QStringLiteral("node"), QStringLiteral("x")}}));
    EXPECT_EQ(ConstraintManager::sidecarMeta(asset).value(QStringLiteral("node")).toString(), QStringLiteral("x"));

    mgr()->clear();
    Ogre::SceneNode* renamed = makeNode("CON_SideOwner2", Ogre::Vector3::ZERO);
    QHash<QString, QString> rename{{QString::fromStdString(owner->getName()), QString::fromStdString(renamed->getName())}};
    QString err;
    ASSERT_EQ(mgr()->loadSidecar(asset, rename, &err), 1) << err.toStdString();
    const Constraint& back = mgr()->constraints().front();
    EXPECT_EQ(back.owner.object, QString::fromStdString(renamed->getName()));
    EXPECT_EQ(back.aimAxis, Axis::NegY);
    EXPECT_DOUBLE_EQ(back.influence, 0.75);

    // Loading again never duplicates ids.
    ASSERT_EQ(mgr()->loadSidecar(asset, rename), 1);
    EXPECT_NE(mgr()->constraints()[0].id, mgr()->constraints()[1].id);
}

TEST_F(ConstraintSceneTest, EmptySidecarIsRemoved)
{
    QTemporaryDir dir;
    const QString asset = dir.filePath(QStringLiteral("a.glb"));
    QFile f(ConstraintManager::sidecarPath(asset));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("{}");
    f.close();
    EXPECT_FALSE(mgr()->writeSidecar(asset));
    EXPECT_FALSE(QFile::exists(ConstraintManager::sidecarPath(asset))) << "a stale sidecar must not resurrect constraints";
}

TEST_F(ConstraintSceneTest, ReplacingTheSceneDropsItsConstraints)
{
    Ogre::SceneNode* owner = makeNode("CON_Clear", Ogre::Vector3::ZERO);
    Ogre::SceneNode* target = makeNode("CON_ClearT", Ogre::Vector3(1, 0, 0));
    ASSERT_TRUE(mgr()->add(make(Type::LookAt, nodeRef(owner), nodeRef(target)), false).ok);
    emit Manager::getSingleton()->sceneClearing();
    EXPECT_EQ(mgr()->count(), 0);
}

TEST_F(ConstraintSceneTest, UiHelpersListObjectsAndAddFromStrings)
{
    Ogre::Entity* e = makeChainEntity("CON_Ui");
    Ogre::SceneNode* target = makeNode("CON_UiTarget", Ogre::Vector3(0.5f, 1, 0));
    EXPECT_TRUE(mgr()->skinnedEntities().contains(QStringLiteral("CON_Ui")));
    EXPECT_EQ(mgr()->bonesOf(QStringLiteral("CON_Ui")), (QStringList{"Root", "Mid", "End"}));
    EXPECT_EQ(mgr()->clipsOf(QStringLiteral("CON_Ui")), QStringList{QStringLiteral("Idle")});
    EXPECT_TRUE(mgr()->nodeNames().contains(QString::fromStdString(target->getName())));
    ASSERT_TRUE(mgr()->addFromUi(QStringLiteral("ik"), QStringLiteral("bone:CON_Ui/End"), nodeRef(target), QString()));
    const QString id = mgr()->constraints().front().id;
    EXPECT_EQ(mgr()->details(id).value(QStringLiteral("type")).toString(), QStringLiteral("ik"));
    EXPECT_TRUE(mgr()->setParamFromUi(id, QStringLiteral("influence"), QStringLiteral("0.5")));
    EXPECT_TRUE(mgr()->bakeFromUi(QStringLiteral("bone:CON_Ui/End"), QStringLiteral("Idle"), 10));
    EXPECT_EQ(e->getSkeleton()->getAnimation("Idle")->getNodeTrack(e->getSkeleton()->getBone("Mid")->getHandle())
                  ->getNumKeyFrames(), 11u);
    EXPECT_TRUE(mgr()->removeFromUi(id));
    EXPECT_FALSE(mgr()->addFromUi(QStringLiteral("wobble"), QStringLiteral("node:x"), QString(), QString()));
}

TEST_F(ConstraintSceneTest, PanelQmlLoadsWithoutErrors)
{
    qmlRegisterSingletonType<AnimationControlController>("AnimationControl", 1, 0, "AnimationControlController",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return AnimationControlController::qmlInstance(e, s); });
    qmlRegisterSingletonType<ConstraintManager>("PropertiesPanel", 1, 0, "ConstraintManager",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return ConstraintManager::qmlInstance(e, s); });
    qmlRegisterSingletonType<PropertiesPanelController>("PropertiesPanel", 1, 0, "PropertiesPanelController",
        [](QQmlEngine* e, QJSEngine*) -> QObject* { return PropertiesPanelController::qmlInstance(e, nullptr); });
    qmlRegisterSingletonType<ThemeManager>("ThemeManager", 1, 0, "ThemeManager",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return ThemeManager::qmlInstance(e, s); });
    QQmlEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/"));
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/AnimationControl/ConstraintsPanel.qml")));
    while (component.isLoading()) QCoreApplication::processEvents();
    ASSERT_FALSE(component.isError()) << component.errorString().toStdString();
    std::unique_ptr<QObject> obj(component.create());
    ASSERT_NE(obj, nullptr) << component.errorString().toStdString();
}
