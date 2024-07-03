#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <QTextDocument>
#include <QTextEdit>
#include <QApplication>
#include "MaterialHighlighter.h"

using ::testing::Mock;

class MockMaterialHighlighter : public MaterialHighlighter
{
public:
    explicit MockMaterialHighlighter(QTextEdit *editor=nullptr){}

    MOCK_METHOD(void, applyHighlight, (const QTextCharFormat &format, const QString &pattern, const QString &text), (override));

    bool containsExpectedFormat(const std::vector<QTextCharFormat>& capturedFormats,
                                const QTextCharFormat& expectedFormat) {
        return std::any_of(capturedFormats.begin(), capturedFormats.end(),
                           [&expectedFormat](const QTextCharFormat& format) {
                               return format.fontWeight() == expectedFormat.fontWeight() &&
                                      format.foreground().color() == expectedFormat.foreground().color();
                           });
    }
};

TEST(MaterialHighlighterTest, HighlightKeywordsDarkModeTest) {
    MockMaterialHighlighter highlighter{};

    QString testText = "material MyMaterial { technique MyTechnique { pass MyPass { } } }";

    QTextCharFormat expectedFormat;
    expectedFormat.setFontWeight(QFont::Bold);
    expectedFormat.setForeground(Qt::darkBlue);

    std::vector<QTextCharFormat> capturedFormats;

    EXPECT_CALL(highlighter, applyHighlight(::testing::_, ::testing::_, testText))
        .Times(5)
        .WillRepeatedly(::testing::Invoke(
            [&capturedFormats](const QTextCharFormat &format, const QString &, const QString &) {
                capturedFormats.push_back(format);
            }
            ));

    highlighter.highlightBlock(testText);

    ASSERT_TRUE(highlighter.containsExpectedFormat(capturedFormats, expectedFormat));
}
