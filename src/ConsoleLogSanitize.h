#ifndef CONSOLELOGSANITIZE_H
#define CONSOLELOGSANITIZE_H

#include <QByteArray>
#include <QString>

namespace ConsoleLogSanitize {

/// Removes common ANSI CSI and OSC sequences (colors, reset, title, etc.).
QString stripAnsiTerminalSequences(const QString& s);

/// Strips ANSI and trims horizontal whitespace from one captured stdio line.
QString sanitizeCapturedStdioLine(const QString& line);

/// Length of the longest valid UTF-8 prefix of `b` (incomplete trailing bytes excluded).
int utf8CompletePrefixLength(const QByteArray& b);

} // namespace ConsoleLogSanitize

#endif
