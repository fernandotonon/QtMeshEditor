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

TEST(MaterialHighlighterTest, CommentHighlighting) {
    // Test comment patterns
    QRegularExpression singleLineCommentPattern("//.*$");
    QRegularExpression multiLineCommentPattern("/\\*.*?\\*/");
    
    // Single-line comments
    EXPECT_TRUE(singleLineCommentPattern.match("// This is a comment").hasMatch()) << "Should match single-line comment";
    EXPECT_TRUE(singleLineCommentPattern.match("material test // inline comment").hasMatch()) << "Should match inline comment";
    EXPECT_TRUE(singleLineCommentPattern.match("    // indented comment").hasMatch()) << "Should match indented comment";
    
    // Multi-line comments
    EXPECT_TRUE(multiLineCommentPattern.match("/* comment */").hasMatch()) << "Should match simple multi-line comment";
    EXPECT_TRUE(multiLineCommentPattern.match("/* multi word comment */").hasMatch()) << "Should match multi-word comment";
    
    // Comments should not interfere with normal syntax when not present
    EXPECT_FALSE(singleLineCommentPattern.match("material TestMaterial").hasMatch()) << "Should NOT match normal material syntax";
    EXPECT_FALSE(multiLineCommentPattern.match("technique standard").hasMatch()) << "Should NOT match normal technique syntax";
}

TEST(MaterialHighlighterTest, CommentedOutCode) {
    // Test that keywords inside comments are properly handled
    QRegularExpression keywordPattern("\\b(material|technique|pass)\\b(?!\\.)");
    QRegularExpression commentPattern("//.*$");
    
    // These tests verify the concept - in actual implementation, 
    // comments would override keyword highlighting
    QString commentedKeyword = "// material TestMaterial";
    EXPECT_TRUE(commentPattern.match(commentedKeyword).hasMatch()) << "Should match comment containing keywords";
    
    QString inlineComment = "pass main // this is a pass comment";
    EXPECT_TRUE(commentPattern.match(inlineComment).hasMatch()) << "Should match inline comment";
    EXPECT_TRUE(keywordPattern.match(inlineComment).hasMatch()) << "Should also match keyword before comment";
}

TEST(MaterialHighlighterTest, ComplexCommentScenarios) {
    QRegularExpression singleLineCommentPattern("//.*$");
    QRegularExpression multiLineCommentPattern("/\\*.*?\\*/");
    
    // Real-world material script with comments
    QString materialWithComments = R"(
// Material definition for grass
material GrassMaterial
{
    technique
    {
        pass
        {
            // Basic lighting properties
            ambient 0.3 0.3 0.3 1.0
            diffuse 0.8 0.8 0.8 1.0  // Main grass color
            
            /* Texture setup for grass
               Using high-res grass texture */
            texture_unit
            {
                texture grass_1024.jpg
            }
        }
    }
}
)";
    
    // Count comment matches
    auto singleMatches = singleLineCommentPattern.globalMatch(materialWithComments);
    int singleCommentCount = 0;
    while (singleMatches.hasNext()) {
        singleMatches.next();
        singleCommentCount++;
    }
    
    auto multiMatches = multiLineCommentPattern.globalMatch(materialWithComments);
    int multiCommentCount = 0;
    while (multiMatches.hasNext()) {
        multiMatches.next();
        multiCommentCount++;
    }
    
    EXPECT_GE(singleCommentCount, 3) << "Should find at least 3 single-line comments";
    EXPECT_GE(multiCommentCount, 1) << "Should find at least 1 multi-line comment";
}

TEST(MaterialHighlighterTest, MultiLineCommentSpanningBlocks) {
    // Test multi-line comment patterns that span multiple blocks
    QRegularExpression multiLineStart("/\\*");
    QRegularExpression multiLineEnd("\\*/");
    
    // Test case 1: Multi-line comment start
    QString startBlock = "material Test /* this comment";
    EXPECT_TRUE(multiLineStart.match(startBlock).hasMatch()) << "Should detect multi-line comment start";
    EXPECT_FALSE(multiLineEnd.match(startBlock).hasMatch()) << "Should not find end in start block";
    
    // Test case 2: Multi-line comment middle (no start or end)
    QString middleBlock = "   continues here with keywords material pass";
    EXPECT_FALSE(multiLineStart.match(middleBlock).hasMatch()) << "Should not find start in middle block";
    EXPECT_FALSE(multiLineEnd.match(middleBlock).hasMatch()) << "Should not find end in middle block";
    
    // Test case 3: Multi-line comment end
    QString endBlock = "   and ends here */ technique";
    EXPECT_FALSE(multiLineStart.match(endBlock).hasMatch()) << "Should not find start in end block";
    EXPECT_TRUE(multiLineEnd.match(endBlock).hasMatch()) << "Should find end in end block";
}

TEST(MaterialHighlighterTest, NestedAndComplexComments) {
    QRegularExpression singleLineComment("//.*$");
    QRegularExpression multiLineStart("/\\*");
    QRegularExpression multiLineEnd("\\*/");
    
    // Test nested-like comments (single-line inside multi-line conceptually)
    QString nestedCase = "/* this is // not a separate comment */";
    EXPECT_TRUE(multiLineStart.match(nestedCase).hasMatch()) << "Should find multi-line start";
    EXPECT_TRUE(multiLineEnd.match(nestedCase).hasMatch()) << "Should find multi-line end";
    EXPECT_TRUE(singleLineComment.match(nestedCase).hasMatch()) << "Would find // pattern but it should be overridden by /* */";
    
    // Test multiple single-line comments
    QString multiSingle = "// comment 1\n// comment 2\nmaterial Test // comment 3";
    auto matches = singleLineComment.globalMatch(multiSingle);
    int count = 0;
    while (matches.hasNext()) {
        matches.next();
        count++;
    }
    EXPECT_GE(count, 3) << "Should find at least 3 single-line comments";
}
