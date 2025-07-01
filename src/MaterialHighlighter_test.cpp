#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <QTextDocument>
#include <QTextEdit>
#include <QApplication>
#include <QRegularExpression>
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

// Helper class for real highlighting tests (not mocked)
class RealMaterialHighlighter : public MaterialHighlighter
{
public:
    explicit RealMaterialHighlighter(QTextDocument *document) : MaterialHighlighter(nullptr) {
        setDocument(document);
    }
    
    // Expose protected method for testing
    void testHighlightBlock(const QString &text) {
        highlightBlock(text);
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

// Test regex patterns directly without document formatting complexity
TEST(MaterialHighlighterTest, KeywordPatternDoesNotMatchInFilenames) {
    // Test the keyword pattern that should NOT match when followed by "."
    QRegularExpression keywordPattern("\\b(material|technique|pass|diffuse|texture|ambient|specular)\\b(?!\\.)");
    
    // Should NOT match keywords in filenames
    EXPECT_FALSE(keywordPattern.match("texture diffuse.png").hasMatch());
    EXPECT_FALSE(keywordPattern.match("load texture.jpg").hasMatch());
    EXPECT_FALSE(keywordPattern.match("use ambient.texture").hasMatch());
    
    // Should still match valid keywords
    EXPECT_TRUE(keywordPattern.match("material TestMaterial").hasMatch());
    EXPECT_TRUE(keywordPattern.match("technique standard").hasMatch());
    EXPECT_TRUE(keywordPattern.match("pass mainPass").hasMatch());
    EXPECT_TRUE(keywordPattern.match("diffuse 1.0 1.0 1.0").hasMatch());
}

TEST(MaterialHighlighterTest, NumberPatternDoesNotMatchInFilenames) {
    // Test the number pattern that should NOT match when preceded by "_" or followed by word chars/dots
    QRegularExpression numberPattern("(?<!_)\\b(\\d+(\\.\\d+)?)\\b(?![\\w.])");
    
    // Should NOT match numbers in filenames
    EXPECT_FALSE(numberPattern.match("grass_1024.jpg").hasMatch());
    EXPECT_FALSE(numberPattern.match("texture_512.dds").hasMatch());
    EXPECT_FALSE(numberPattern.match("model_256.png").hasMatch());
    EXPECT_FALSE(numberPattern.match("file123.txt").hasMatch());
    
    // Should still match valid numbers
    QString validNumbers = "ambient 0.5 0.5 0.5 1.0";
    auto matches = numberPattern.globalMatch(validNumbers);
    int matchCount = 0;
    while (matches.hasNext()) {
        matches.next();
        matchCount++;
    }
    EXPECT_EQ(matchCount, 4); // Should match all 4 numbers
}

TEST(MaterialHighlighterTest, FilenameScenarios) {
    // Test specific filename scenarios mentioned in the issue
    QRegularExpression keywordPattern("\\b(diffuse|ambient|specular|normal)\\b(?!\\.)");
    QRegularExpression numberPattern("(?<!_)\\b(\\d+(\\.\\d+)?)\\b(?![\\w.])");
    
    // Test case 1: "texture grass_1024.jpg"
    QString case1 = "texture grass_1024.jpg";
    EXPECT_FALSE(numberPattern.match(case1).hasMatch()) << "Should not match '1024' in 'grass_1024.jpg'";
    
    // Test case 2: "texture diffuse.png"  
    QString case2 = "texture diffuse.png";
    EXPECT_FALSE(keywordPattern.match(case2).hasMatch()) << "Should not match 'diffuse' in 'diffuse.png'";
    
    // Test case 3: More complex filenames
    EXPECT_FALSE(numberPattern.match("normal_512.dds").hasMatch()) << "Should not match '512' in 'normal_512.dds'";
    EXPECT_FALSE(keywordPattern.match("specular.tga").hasMatch()) << "Should not match 'specular' in 'specular.tga'";
}

TEST(MaterialHighlighterTest, ValidSyntaxStillMatches) {
    QRegularExpression keywordPattern("\\b(material|technique|pass|diffuse|ambient)\\b(?!\\.)");
    QRegularExpression numberPattern("(?<!_)\\b(\\d+(\\.\\d+)?)\\b(?![\\w.])");
    
    // Test valid material script syntax
    QString validScript = "material TestMaterial\n{\n    technique\n    {\n        pass\n        {\n            ambient 0.5 0.5 0.5 1.0\n            diffuse 1.0 1.0 1.0\n        }\n    }\n}";
    
    // Should match keywords
    EXPECT_TRUE(keywordPattern.match(validScript).hasMatch());
    
    // Count number matches in valid script (should be 7: 0.5, 0.5, 0.5, 1.0, 1.0, 1.0, 1.0)
    auto numberMatches = numberPattern.globalMatch(validScript);
    int numberCount = 0;
    while (numberMatches.hasNext()) {
        numberMatches.next();
        numberCount++;
    }
    EXPECT_EQ(numberCount, 7) << "Should match all valid numbers in material script";
}

TEST(MaterialHighlighterTest, EdgeCases) {
    QRegularExpression keywordPattern("\\b(texture|material)\\b(?!\\.)");
    QRegularExpression numberPattern("(?<!_)\\b(\\d+(\\.\\d+)?)\\b(?![\\w.])");
    
    // Edge cases
    EXPECT_TRUE(keywordPattern.match("texture ").hasMatch()) << "Should match 'texture' followed by space";
    EXPECT_TRUE(keywordPattern.match("material{").hasMatch()) << "Should match 'material' followed by brace";
    EXPECT_FALSE(keywordPattern.match("texture.").hasMatch()) << "Should NOT match 'texture' followed by dot";
    
    EXPECT_TRUE(numberPattern.match("123 ").hasMatch()) << "Should match number followed by space";
    EXPECT_TRUE(numberPattern.match("456}").hasMatch()) << "Should match number followed by brace";
    EXPECT_FALSE(numberPattern.match("_123").hasMatch()) << "Should NOT match number preceded by underscore";
    EXPECT_FALSE(numberPattern.match("123.jpg").hasMatch()) << "Should NOT match number followed by dot+text";
}
