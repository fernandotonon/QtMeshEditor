#include <gtest/gtest.h>
#include <QSignalSpy>
#include <QObject>
#include "QMLMaterialHighlighter.h"

class QMLMaterialHighlighterTest : public ::testing::Test {
protected:
    void SetUp() override {
        highlighter = new QMLMaterialHighlighter();
    }
    void TearDown() override {
        delete highlighter;
        highlighter = nullptr;
    }
    QMLMaterialHighlighter* highlighter = nullptr;
};

// Test 1: Constructor initializes document to nullptr
TEST_F(QMLMaterialHighlighterTest, Constructor_InitialState) {
    EXPECT_EQ(highlighter->document(), nullptr);
}

// Test 2: setDocument(nullptr) when already nullptr triggers early return (no signal)
TEST_F(QMLMaterialHighlighterTest, SetDocument_NullToNull_NoSignal) {
    QSignalSpy spy(highlighter, &QMLMaterialHighlighter::documentChanged);
    ASSERT_TRUE(spy.isValid());

    highlighter->setDocument(nullptr);

    // m_document is already nullptr, so the guard (m_document == document) returns early
    EXPECT_EQ(spy.count(), 0);
    EXPECT_EQ(highlighter->document(), nullptr);
}

// Test 3: Calling setDocument(nullptr) does not crash even on a fresh instance
TEST_F(QMLMaterialHighlighterTest, SetDocument_NullPtr_NoCrash) {
    EXPECT_NO_FATAL_FAILURE(highlighter->setDocument(nullptr));
    EXPECT_EQ(highlighter->document(), nullptr);
}

// Test 4: Destroying without ever setting a document does not crash
TEST_F(QMLMaterialHighlighterTest, Destructor_WithoutDocument) {
    auto* h = new QMLMaterialHighlighter();
    EXPECT_NO_FATAL_FAILURE(delete h);
}

// Test 5: Parent ownership is respected through QObject hierarchy
TEST_F(QMLMaterialHighlighterTest, Destructor_WithParent) {
    auto* parent = new QObject();
    auto* child = new QMLMaterialHighlighter(parent);

    EXPECT_EQ(child->parent(), parent);
    EXPECT_EQ(child->document(), nullptr);

    // Deleting parent should also delete the child (QObject ownership)
    EXPECT_NO_FATAL_FAILURE(delete parent);
}

// Test 6: document() getter consistently returns nullptr when nothing is set
TEST_F(QMLMaterialHighlighterTest, DocumentProperty_ReturnsNull) {
    // Call getter multiple times - should always be nullptr
    EXPECT_EQ(highlighter->document(), nullptr);
    EXPECT_EQ(highlighter->document(), nullptr);

    // After a no-op setDocument, still nullptr
    highlighter->setDocument(nullptr);
    EXPECT_EQ(highlighter->document(), nullptr);
}

// Test 7: Multiple instances do not interfere with each other
TEST_F(QMLMaterialHighlighterTest, MultipleInstances) {
    auto* h1 = new QMLMaterialHighlighter();
    auto* h2 = new QMLMaterialHighlighter();

    EXPECT_EQ(h1->document(), nullptr);
    EXPECT_EQ(h2->document(), nullptr);

    // Ensure they are distinct objects
    EXPECT_NE(h1, h2);

    delete h1;
    delete h2;

    // The fixture's highlighter should be unaffected
    EXPECT_EQ(highlighter->document(), nullptr);
}
