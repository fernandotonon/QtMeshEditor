#ifndef CONSOLELOGSANITIZE_H
#define CONSOLELOGSANITIZE_H

#include <QByteArray>
#include <QString>

namespace ConsoleLogSanitize {

/// Removes common ANSI CSI and OSC sequences (colors, reset, title, etc.).
QString stripAnsiTerminalSequences(const QString& s);

/// Strips ANSI and trims horizontal whitespace from one captured stdio line.
QString sanitizeCapturedStdioLine(const QString& line);

/// Length of the longest decodable prefix of `b` for streaming UTF-8: complete code points
/// plus any truncated final sequence (returned length excludes incomplete tail). On malformed
/// multibyte sequences, skips one byte and resyncs so callers never spin on a fixed carry.
int utf8CompletePrefixLength(const QByteArray& b);

} // namespace ConsoleLogSanitize

#endif
