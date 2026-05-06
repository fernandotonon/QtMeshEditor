#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>

#include "CurveEditModelChangeCommand.h"
#include "../CurveEditModel.h"

class CurveEditModelChangeCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        CurveEditModel::kill();
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
    }
    void TearDown() override {
        CurveEditModel::kill();
    }
    QApplication* app = nullptr;
};

TEST_F(CurveEditModelChangeCommandTest, RedoAppliesNewState) {
    CurveEditModelChangeCommand cmd("s", "a", "b", "tx", 0.5,
                                     0.0, 0.0, CurveEditModel::ModeBezier,
                                     1.5, -0.75, CurveEditModel::ModeLinear);
    cmd.redo();

    auto out = CurveEditModel::instance()->tangentsAt("s", "a", "b", "tx", 0.5);
    EXPECT_DOUBLE_EQ(out[0].toDouble(), 1.5);
    EXPECT_DOUBLE_EQ(out[1].toDouble(), -0.75);
    EXPECT_EQ(out[2].toInt(), CurveEditModel::ModeLinear);
}

TEST_F(CurveEditModelChangeCommandTest, UndoRestoresOldState) {
    // Pre-populate with the "old" state so undo lands somewhere
    // observable (rather than the implicit default Bezier 0/0).
    auto* m = CurveEditModel::instance();
    m->setTangents("s", "a", "b", "tx", 0.5, 2.0, 3.0);
    m->setMode("s", "a", "b", "tx", 0.5, CurveEditModel::ModeAuto);

    CurveEditModelChangeCommand cmd("s", "a", "b", "tx", 0.5,
                                     2.0, 3.0, CurveEditModel::ModeAuto,
                                     1.5, -0.75, CurveEditModel::ModeLinear);
    cmd.redo();
    cmd.undo();

    auto out = m->tangentsAt("s", "a", "b", "tx", 0.5);
    EXPECT_DOUBLE_EQ(out[0].toDouble(), 2.0);
    EXPECT_DOUBLE_EQ(out[1].toDouble(), 3.0);
    EXPECT_EQ(out[2].toInt(), CurveEditModel::ModeAuto);
}

TEST_F(CurveEditModelChangeCommandTest, RedoUndoRoundTripIsStable) {
    // Repeated redo/undo cycles must converge to the same two states.
    CurveEditModelChangeCommand cmd("s", "a", "b", "tx", 0.5,
                                     0.0, 0.0, CurveEditModel::ModeBezier,
                                     1.0, 1.0, CurveEditModel::ModeStepped);
    auto* m = CurveEditModel::instance();
    cmd.redo();
    auto afterRedo1 = m->tangentsAt("s", "a", "b", "tx", 0.5);
    cmd.undo();
    auto afterUndo1 = m->tangentsAt("s", "a", "b", "tx", 0.5);
    cmd.redo();
    auto afterRedo2 = m->tangentsAt("s", "a", "b", "tx", 0.5);
    cmd.undo();
    auto afterUndo2 = m->tangentsAt("s", "a", "b", "tx", 0.5);

    EXPECT_EQ(afterRedo1[2].toInt(), afterRedo2[2].toInt());
    EXPECT_EQ(afterUndo1[2].toInt(), afterUndo2[2].toInt());
    EXPECT_EQ(afterRedo1[0].toDouble(), afterRedo2[0].toDouble());
    EXPECT_EQ(afterUndo1[0].toDouble(), afterUndo2[0].toDouble());
}
