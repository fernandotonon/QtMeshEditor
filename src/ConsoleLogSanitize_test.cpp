#include <gtest/gtest.h>
#include "ConsoleLogSanitize.h"

#include <QString>

using namespace ConsoleLogSanitize;

TEST(ConsoleLogSanitize, StripAnsiRemovesCsiColorAndReset)
{
    const QString raw = QStringLiteral("hello \x1B[31mworld\x1B[0m");
    EXPECT_EQ(stripAnsiTerminalSequences(raw), QStringLiteral("hello world"));
}

TEST(ConsoleLogSanitize, StripAnsiRemovesStandaloneReset)
{
    const QString raw = QStringLiteral("done\x1B[0m");
    EXPECT_EQ(stripAnsiTerminalSequences(raw), QStringLiteral("done"));
}

TEST(ConsoleLogSanitize, StripAnsiRemovesOscWindowTitleBelTerminated)
{
    const QString raw = QStringLiteral("x\x1B]0;My Title\x07y");
    EXPECT_EQ(stripAnsiTerminalSequences(raw), QStringLiteral("xy"));
}

TEST(ConsoleLogSanitize, SanitizeTrimsAndStripsAnsi)
{
    const QString raw = QStringLiteral("  msg\x1B[0m  ");
    EXPECT_EQ(sanitizeCapturedStdioLine(raw), QStringLiteral("msg"));
}

TEST(ConsoleLogSanitize, Utf8CompletePrefixDecodesFullSequence)
{
    const QByteArray utf8Char = QByteArrayLiteral("a\xc3\xa9"); // a + é (U+00E9)
    EXPECT_EQ(utf8CompletePrefixLength(utf8Char), 3);
}

TEST(ConsoleLogSanitize, Utf8CompletePrefixExcludesIncompleteTrailingBytes)
{
    const QByteArray incomplete = QByteArrayLiteral("a\xc3");
    EXPECT_EQ(utf8CompletePrefixLength(incomplete), 1);
}

/// Invalid UTF-8 lead bytes are skipped one byte at a time until the buffer end.
TEST(ConsoleLogSanitize, Utf8CompletePrefixAdvancesPastInvalidBytes)
{
    EXPECT_EQ(utf8CompletePrefixLength(QByteArrayLiteral("\xff")), 1);
    EXPECT_EQ(utf8CompletePrefixLength(QByteArrayLiteral("\xff\x80")), 2);
}
