#include "ConsoleLogSanitize.h"

#include <QRegularExpression>

namespace ConsoleLogSanitize {

QString stripAnsiTerminalSequences(const QString& s)
{
    QString out = s;
    static const QRegularExpression csi(QStringLiteral(R"(\x1B\[[\x30-\x3F]*[\x20-\x2F]*[\x40-\x7E])"));
    out.remove(csi);
    static const QRegularExpression oscBel(QStringLiteral(R"(\x1B\][^\x07]*\x07)"));
    out.remove(oscBel);
    static const QRegularExpression oscSt(QStringLiteral(R"(\x1B\][^\x1B]*\x1B\\)"));
    out.remove(oscSt);
    return out;
}

QString sanitizeCapturedStdioLine(const QString& line)
{
    return stripAnsiTerminalSequences(line).trimmed();
}

int utf8CompletePrefixLength(const QByteArray& b)
{
    int i = 0;
    const int n = b.size();
    while (i < n) {
        const auto u = static_cast<unsigned char>(b.at(i));
        int need = 1;
        if (u < 0x80) {
            need = 1;
        } else if ((u & 0xE0) == 0xC0) {
            need = 2;
        } else if ((u & 0xF0) == 0xE0) {
            need = 3;
        } else if ((u & 0xF8) == 0xF0) {
            need = 4;
        } else {
            ++i;
            continue;
        }
        if (i + need > n)
            return i;
        bool badContinuation = false;
        for (int j = 1; j < need; ++j) {
            if ((static_cast<unsigned char>(b.at(i + j)) & 0xC0) != 0x80) {
                badContinuation = true;
                break;
            }
        }
        if (badContinuation) {
            // Resync: do not return i (that would leave the bad lead in carry forever in
            // streaming decode). Skip one byte and continue like an invalid lead.
            ++i;
            continue;
        }
        i += need;
    }
    return n;
}

} // namespace ConsoleLogSanitize
