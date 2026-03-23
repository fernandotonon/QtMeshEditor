#include <gtest/gtest.h>
#include "UndoManager.h"
#include <QUndoCommand>

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
