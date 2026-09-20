#include "NpzReader.h"

#include <QFile>

#include <cstring>

namespace AudioToFace {

namespace {

// Zip local-file-header and central-directory constants.
constexpr quint32 kCentralSig = 0x02014b50;
constexpr quint32 kEocdSig    = 0x06054b50;
constexpr quint32 kLocalSig   = 0x04034b50;

quint16 rd16(const char* p)
{
    return quint16(quint8(p[0])) | (quint16(quint8(p[1])) << 8);
}
quint32 rd32(const char* p)
{
    return quint32(quint8(p[0])) | (quint32(quint8(p[1])) << 8) |
           (quint32(quint8(p[2])) << 16) | (quint32(quint8(p[3])) << 24);
}

/// Pull one `key: value` out of the `.npy` header dict. The header is a
/// Python literal, but only three fields matter, so a scan beats a parser.
QString headerField(const QString& header, const QString& key)
{
    const int k = header.indexOf(QStringLiteral("'%1'").arg(key));
    if (k < 0) return {};
    int c = header.indexOf(QLatin1Char(':'), k);
    if (c < 0) return {};
    ++c;
    while (c < header.size() && header[c].isSpace()) ++c;
    int end = c;
    int depth = 0;
    while (end < header.size()) {
        const QChar ch = header[end];
        if (ch == QLatin1Char('(') || ch == QLatin1Char('[')) ++depth;
        else if (ch == QLatin1Char(')') || ch == QLatin1Char(']')) --depth;
        else if (ch == QLatin1Char(',') && depth <= 0) break;
        ++end;
    }
    return header.mid(c, end - c).trimmed();
}

}  // namespace

int64_t NpyArray::elementCount() const
{
    int64_t n = 1;
    for (int64_t d : shape) n *= d;
    return shape.empty() ? 0 : n;
}

bool NpzReader::open(const QString& path, QString* error)
{
    m_open = false;
    m_entries.clear();
    m_path = path;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot open %1").arg(path);
        return false;
    }

    // Find the end-of-central-directory record by scanning back from the end.
    // The comment field is almost always empty, so the record sits in the last
    // 22 bytes; 64 KiB is the maximum a comment can push it back by.
    const qint64 fileSize = f.size();
    const qint64 tailLen = qMin<qint64>(fileSize, 66000);
    if (!f.seek(fileSize - tailLen)) {
        if (error) *error = QStringLiteral("seek failed");
        return false;
    }
    const QByteArray tail = f.read(tailLen);
    int eocd = -1;
    for (int i = tail.size() - 22; i >= 0; --i) {
        if (rd32(tail.constData() + i) == kEocdSig) { eocd = i; break; }
    }
    if (eocd < 0) {
        if (error) *error = QStringLiteral("not a zip archive (no end-of-central-directory)");
        return false;
    }
    const char* e = tail.constData() + eocd;
    const quint16 count = rd16(e + 10);
    const quint32 cdSize = rd32(e + 12);
    const quint32 cdOffset = rd32(e + 16);

    if (!f.seek(cdOffset)) {
        if (error) *error = QStringLiteral("central directory offset out of range");
        return false;
    }
    const QByteArray cd = f.read(cdSize);
    if (qint64(cd.size()) != qint64(cdSize)) {
        if (error) *error = QStringLiteral("truncated central directory");
        return false;
    }

    int p = 0;
    for (int i = 0; i < int(count); ++i) {
        if (p + 46 > cd.size() || rd32(cd.constData() + p) != kCentralSig) break;
        const char* h = cd.constData() + p;
        const quint16 method   = rd16(h + 10);
        const quint32 compSize = rd32(h + 20);
        const quint32 rawSize  = rd32(h + 24);
        const quint16 nameLen  = rd16(h + 28);
        const quint16 extraLen = rd16(h + 30);
        const quint16 cmtLen   = rd16(h + 32);
        const quint32 localOfs = rd32(h + 42);
        if (p + 46 + nameLen > cd.size()) break;

        Entry en;
        en.name = QString::fromUtf8(h + 46, nameLen);
        if (en.name.endsWith(QStringLiteral(".npy")))
            en.name.chop(4);
        en.deflated = (method != 0);
        en.size = en.deflated ? qint64(compSize) : qint64(rawSize);

        // The central directory records where the LOCAL header is; the payload
        // sits past that header's own variable-length name/extra fields, which
        // may differ from the central copy's.
        if (f.seek(localOfs)) {
            const QByteArray lh = f.read(30);
            if (lh.size() == 30 && rd32(lh.constData()) == kLocalSig) {
                const quint16 lNameLen  = rd16(lh.constData() + 26);
                const quint16 lExtraLen = rd16(lh.constData() + 28);
                en.dataOffset = qint64(localOfs) + 30 + lNameLen + lExtraLen;
                m_entries.push_back(en);
            }
        }
        p += 46 + nameLen + extraLen + cmtLen;
    }

    if (m_entries.empty()) {
        if (error) *error = QStringLiteral("archive contains no readable members");
        return false;
    }
    m_open = true;
    return true;
}

QStringList NpzReader::names() const
{
    QStringList out;
    out.reserve(int(m_entries.size()));
    for (const auto& e : m_entries) out << e.name;
    return out;
}

bool NpzReader::contains(const QString& name) const { return find(name) != nullptr; }

const NpzReader::Entry* NpzReader::find(const QString& name) const
{
    for (const auto& e : m_entries)
        if (e.name == name) return &e;
    return nullptr;
}

QByteArray NpzReader::readRaw(const Entry& e, QString* error) const
{
    if (e.deflated) {
        // Both shipped archives are STORED, so this path is unreachable today.
        // Reported explicitly rather than skipped: if a re-upload ever
        // compresses them, a silently missing array would look like a broken
        // model rather than a format change.
        if (error)
            *error = QStringLiteral("member '%1' is DEFLATE-compressed; only stored "
                                    "members are supported").arg(e.name);
        return {};
    }
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(e.dataOffset)) {
        if (error) *error = QStringLiteral("cannot read member '%1'").arg(e.name);
        return {};
    }
    QByteArray raw = f.read(e.size);
    if (qint64(raw.size()) != e.size) {
        if (error) *error = QStringLiteral("member '%1' is truncated").arg(e.name);
        return {};
    }
    return raw;
}

NpyArray NpzReader::read(const QString& name, QString* error) const
{
    NpyArray out;
    const Entry* e = find(name);
    if (!e) {
        if (error) *error = QStringLiteral("no member named '%1'").arg(name);
        return out;
    }
    const QByteArray raw = readRaw(*e, error);
    if (raw.isEmpty()) return out;
    if (raw.size() < 10 || std::memcmp(raw.constData(), "\x93NUMPY", 6) != 0) {
        if (error) *error = QStringLiteral("member '%1' is not a .npy array").arg(name);
        return out;
    }
    const quint8 major = quint8(raw[6]);
    int hlenBytes = (major == 1) ? 2 : 4;
    if (raw.size() < 8 + hlenBytes) {
        if (error) *error = QStringLiteral("member '%1' has a truncated header").arg(name);
        return out;
    }
    const quint32 hlen = (major == 1) ? rd16(raw.constData() + 8)
                                      : rd32(raw.constData() + 8);
    const int dataStart = 8 + hlenBytes + int(hlen);
    if (raw.size() < dataStart) {
        if (error) *error = QStringLiteral("member '%1' header overruns the payload").arg(name);
        return out;
    }
    const QString header = QString::fromLatin1(raw.constData() + 8 + hlenBytes, int(hlen));

    // dtype: only little-endian float32. Anything else is refused BY NAME
    // rather than reinterpreted — reading float64 as float32 produces
    // plausible-looking garbage that would surface as a strange face.
    const QString descr = headerField(header, QStringLiteral("descr"))
                              .remove(QLatin1Char('\'')).remove(QLatin1Char('"'));
    if (descr != QStringLiteral("<f4")) {
        if (error)
            *error = QStringLiteral("member '%1' has dtype %2; only '<f4' is supported")
                         .arg(name, descr);
        return out;
    }
    if (headerField(header, QStringLiteral("fortran_order")) == QStringLiteral("True")) {
        if (error)
            *error = QStringLiteral("member '%1' is Fortran-ordered; only C order is "
                                    "supported").arg(name);
        return out;
    }

    const QString shapeStr = headerField(header, QStringLiteral("shape"));
    for (const QString& part : shapeStr.mid(1, shapeStr.size() - 2)
                                   .split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool ok = false;
        const qlonglong v = part.trimmed().toLongLong(&ok);
        if (ok) out.shape.push_back(int64_t(v));
    }

    const int64_t want = out.elementCount();
    const int64_t have = int64_t(raw.size() - dataStart) / 4;
    if (want <= 0 || have < want) {
        if (error)
            *error = QStringLiteral("member '%1' declares %2 elements but holds %3")
                         .arg(name).arg(want).arg(have);
        out.shape.clear();
        return out;
    }
    out.data.resize(size_t(want));
    std::memcpy(out.data.data(), raw.constData() + dataStart, size_t(want) * 4);
    return out;
}

QStringList NpzReader::readStrings(const QString& name, QString* error) const
{
    QStringList out;
    const Entry* e = find(name);
    if (!e) {
        if (error) *error = QStringLiteral("no member named '%1'").arg(name);
        return out;
    }
    const QByteArray raw = readRaw(*e, error);
    if (raw.isEmpty()) return out;
    if (raw.size() < 10 || std::memcmp(raw.constData(), "\x93NUMPY", 6) != 0) {
        if (error) *error = QStringLiteral("member '%1' is not a .npy array").arg(name);
        return out;
    }
    const quint8 major = quint8(raw[6]);
    const int hlenBytes = (major == 1) ? 2 : 4;
    const quint32 hlen = (major == 1) ? rd16(raw.constData() + 8)
                                      : rd32(raw.constData() + 8);
    const int dataStart = 8 + hlenBytes + int(hlen);
    if (raw.size() < dataStart) return out;
    const QString header = QString::fromLatin1(raw.constData() + 8 + hlenBytes, int(hlen));

    // Fixed-width byte strings: '|S19' means 19 bytes per entry, NUL-padded.
    const QString descr = headerField(header, QStringLiteral("descr"))
                              .remove(QLatin1Char('\'')).remove(QLatin1Char('"'));
    if (!descr.startsWith(QStringLiteral("|S"))) {
        if (error)
            *error = QStringLiteral("member '%1' has dtype %2; expected a byte-string "
                                    "array").arg(name, descr);
        return out;
    }
    bool ok = false;
    const int width = descr.mid(2).toInt(&ok);
    if (!ok || width <= 0) return out;

    const QString shapeStr = headerField(header, QStringLiteral("shape"));
    int count = 0;
    for (const QString& part : shapeStr.mid(1, shapeStr.size() - 2)
                                   .split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool k = false;
        const int v = part.trimmed().toInt(&k);
        if (k) count = (count == 0) ? v : count * v;
    }
    for (int i = 0; i < count; ++i) {
        const int off = dataStart + i * width;
        if (off + width > raw.size()) break;
        QByteArray s(raw.constData() + off, width);
        const int nul = s.indexOf('\0');
        if (nul >= 0) s.truncate(nul);
        out << QString::fromLatin1(s);
    }
    return out;
}

}  // namespace AudioToFace
