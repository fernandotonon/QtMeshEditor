#include <gtest/gtest.h>
#include "UndoManager.h"
#include <QUndoCommand>
#include <QSignalSpy>

class UndoManagerTests : public ::testing::Test {
protected:
    void SetUp() override {
        UndoManager::getSingleton()->clear();
    }
    void TearDown() override {
        UndoManager::getSingleton()->clear();
    }
};

class TestCommand : public QUndoCommand {
public:
    TestCommand(int* counter, int delta)
        : QUndoCommand("Test"), m_counter(counter), m_delta(delta) {}
    void undo() override { *m_counter -= m_delta; }
    void redo() override { *m_counter += m_delta; }
private:
    int* m_counter;
    int m_delta;
};

TEST_F(UndoManagerTests, Singleton) {
    ASSERT_NE(UndoManager::getSingleton(), nullptr);
    EXPECT_EQ(UndoManager::getSingleton(), UndoManager::getSingleton());
}

TEST_F(UndoManagerTests, PushAndUndo) {
    int counter = 0;
    UndoManager::getSingleton()->push(new TestCommand(&counter, 5));
    EXPECT_EQ(counter, 5);

    UndoManager::getSingleton()->undo();
    EXPECT_EQ(counter, 0);
}

TEST_F(UndoManagerTests, PushAndRedo) {
    int counter = 0;
    UndoManager::getSingleton()->push(new TestCommand(&counter, 3));
    EXPECT_EQ(counter, 3);

    UndoManager::getSingleton()->undo();
    EXPECT_EQ(counter, 0);

    UndoManager::getSingleton()->redo();
    EXPECT_EQ(counter, 3);
}

TEST_F(UndoManagerTests, CanUndoRedo) {
    EXPECT_FALSE(UndoManager::getSingleton()->canUndo());
    EXPECT_FALSE(UndoManager::getSingleton()->canRedo());

    int counter = 0;
    UndoManager::getSingleton()->push(new TestCommand(&counter, 1));
    EXPECT_TRUE(UndoManager::getSingleton()->canUndo());
    EXPECT_FALSE(UndoManager::getSingleton()->canRedo());

    UndoManager::getSingleton()->undo();
    EXPECT_FALSE(UndoManager::getSingleton()->canUndo());
    EXPECT_TRUE(UndoManager::getSingleton()->canRedo());
}

TEST_F(UndoManagerTests, Clear) {
    int counter = 0;
    UndoManager::getSingleton()->push(new TestCommand(&counter, 1));
    UndoManager::getSingleton()->clear();
    EXPECT_FALSE(UndoManager::getSingleton()->canUndo());
    EXPECT_FALSE(UndoManager::getSingleton()->canRedo());
}

TEST_F(UndoManagerTests, MultipleCommands) {
    int counter = 0;
    UndoManager::getSingleton()->push(new TestCommand(&counter, 1));
    UndoManager::getSingleton()->push(new TestCommand(&counter, 2));
    UndoManager::getSingleton()->push(new TestCommand(&counter, 3));
    EXPECT_EQ(counter, 6);

    UndoManager::getSingleton()->undo();
    EXPECT_EQ(counter, 3);

    UndoManager::getSingleton()->undo();
    EXPECT_EQ(counter, 1);

    UndoManager::getSingleton()->redo();
    EXPECT_EQ(counter, 3);
}

TEST_F(UndoManagerTests, StackReturnsNonNull) {
    EXPECT_NE(UndoManager::getSingleton()->stack(), nullptr);
}

TEST_F(UndoManagerTests, PushManyAndUndoAll) {
    int counter = 0;
    const int numCommands = 15;

    for (int i = 1; i <= numCommands; ++i) {
        UndoManager::getSingleton()->push(new TestCommand(&counter, i));
    }
    // Sum of 1..15 = 120
    EXPECT_EQ(counter, 120);

    // Undo all
    for (int i = 0; i < numCommands; ++i) {
        EXPECT_TRUE(UndoManager::getSingleton()->canUndo());
        UndoManager::getSingleton()->undo();
    }
    EXPECT_EQ(counter, 0);
    EXPECT_FALSE(UndoManager::getSingleton()->canUndo());
    EXPECT_TRUE(UndoManager::getSingleton()->canRedo());
}

TEST_F(UndoManagerTests, RedoClearedByNewPushAfterUndo) {
    int counter = 0;
    UndoManager::getSingleton()->push(new TestCommand(&counter, 10));
    UndoManager::getSingleton()->push(new TestCommand(&counter, 20));
    EXPECT_EQ(counter, 30);

    // Undo the last command
    UndoManager::getSingleton()->undo();
    EXPECT_EQ(counter, 10);
    EXPECT_TRUE(UndoManager::getSingleton()->canRedo());

    // Push a new command -- this should clear the redo stack
    UndoManager::getSingleton()->push(new TestCommand(&counter, 5));
    EXPECT_EQ(counter, 15);
    EXPECT_FALSE(UndoManager::getSingleton()->canRedo());

    // Undo should give us back 10
    UndoManager::getSingleton()->undo();
    EXPECT_EQ(counter, 10);

    // Redo should give us 15 (the new command), not 30 (the old one)
    UndoManager::getSingleton()->redo();
    EXPECT_EQ(counter, 15);
}

TEST_F(UndoManagerTests, UndoTextChangedSignal) {
    QSignalSpy spy(UndoManager::getSingleton(), &UndoManager::undoTextChanged);
    int counter = 0;
    UndoManager::getSingleton()->push(new TestCommand(&counter, 1));
    EXPECT_GE(spy.count(), 1);
}

TEST_F(UndoManagerTests, RedoTextChangedSignal) {
    int counter = 0;
    UndoManager::getSingleton()->push(new TestCommand(&counter, 1));

    QSignalSpy spy(UndoManager::getSingleton(), &UndoManager::redoTextChanged);
    UndoManager::getSingleton()->undo();
    EXPECT_GE(spy.count(), 1);
}

TEST_F(UndoManagerTests, UndoOnEmptyStackDoesNotCrash) {
    EXPECT_FALSE(UndoManager::getSingleton()->canUndo());
    EXPECT_NO_THROW(UndoManager::getSingleton()->undo());
}

TEST_F(UndoManagerTests, RedoOnEmptyStackDoesNotCrash) {
    EXPECT_FALSE(UndoManager::getSingleton()->canRedo());
    EXPECT_NO_THROW(UndoManager::getSingleton()->redo());
}

TEST_F(UndoManagerTests, KillAndRecreate) {
    int counter = 0;
    UndoManager::getSingleton()->push(new TestCommand(&counter, 42));
    EXPECT_TRUE(UndoManager::getSingleton()->canUndo());

    UndoManager::kill();

    // After kill, getSingleton creates a fresh instance
    EXPECT_NE(UndoManager::getSingleton(), nullptr);
    EXPECT_FALSE(UndoManager::getSingleton()->canUndo());
    EXPECT_FALSE(UndoManager::getSingleton()->canRedo());
}
