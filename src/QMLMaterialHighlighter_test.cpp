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
// Test 4: Destroying without ever setting a document does not crash
// Test 5: Parent ownership is respected through QObject hierarchy
// Test 6: document() getter consistently returns nullptr when nothing is set
// Test 7: Multiple instances do not interfere with each other