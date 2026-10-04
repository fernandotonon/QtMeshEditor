// Audio2Face (#1019) .npz reader. Builds real archives on disk rather than
// mocking, since the point is to parse the zip/npy byte layout correctly.
//
// Verified separately against NVIDIA's shipped archives, which is the check
// that actually matters:
//   shapes_matrix_skin  [272,61520,3]  50,200,320 floats, 0 non-finite
//   shapes_mean_skin    [61520,3]      mean|v| 54.5926
//   neutral (bs_skin)   [61520,3]      mean|v| 54.5881  <- same rest face
//   jawOpen             [61520,3]      min -3.6432      <- jaw dropping
//   poseNames           53 entries, 'neutral' .. 'tongueOut'
// Those files are a 240 MB download, so the unit tests use synthetic archives
// with the same byte layout.
#include <gtest/gtest.h>

#include "AudioToFace/NpzReader.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstring>
#include <vector>

using AudioToFace::NpzReader;

namespace {

void put16(QByteArray& b, quint16 v) { b.append(char(v & 0xFF)); b.append(char(v >> 8)); }
void put32(QByteArray& b, quint32 v)
{
    for (int i = 0; i < 4; ++i) b.append(char((v >> (8*i)) & 0xFF));
}

/// A `.npy` payload: magic, version, ASCII dict header, then raw data.
QByteArray npy(const QByteArray& descr, const QByteArray& shape,
               const QByteArray& payload, bool fortran = false)
{
    QByteArray hdr = "{'descr': '" + descr + "', 'fortran_order': " +
                     (fortran ? "True" : "False") + ", 'shape': " + shape + ", }";
    while ((10 + hdr.size()) % 64 != 0) hdr.append(' ');
    hdr.append('\n');
    QByteArray out("\x93NUMPY", 6);
    out.append(char(1)); out.append(char(0));
    put16(out, quint16(hdr.size()));
    out.append(hdr);
    out.append(payload);
    return out;
}

/// A zip with STORED members — the layout NumPy writes and NVIDIA ships.
bool writeNpz(const QString& path, const std::vector<std::pair<QByteArray, QByteArray>>& members)
{
    QByteArray out;
    struct Rec { QByteArray name; quint32 ofs, size; };
    std::vector<Rec> recs;
    for (const auto& m : members) {
        Rec r; r.name = m.first + ".npy"; r.ofs = quint32(out.size());
        r.size = quint32(m.second.size());
        out.append("PK\x03\x04", 4);
        put16(out, 20); put16(out, 0); put16(out, 0);   // version, flags, STORED
        put16(out, 0);  put16(out, 0);                  // time, date
        put32(out, 0);                                  // crc (unchecked by the reader)
        put32(out, r.size); put32(out, r.size);
        put16(out, quint16(r.name.size())); put16(out, 0);
        out.append(r.name);
        out.append(m.second);
        recs.push_back(r);
    }
    const quint32 cdStart = quint32(out.size());
    for (const auto& r : recs) {
        out.append("PK\x01\x02", 4);
        put16(out, 20); put16(out, 20); put16(out, 0); put16(out, 0);
        put16(out, 0); put16(out, 0);
        put32(out, 0); put32(out, r.size); put32(out, r.size);
        put16(out, quint16(r.name.size()));
        put16(out, 0); put16(out, 0); put16(out, 0); put16(out, 0);
        put32(out, 0); put32(out, r.ofs);
        out.append(r.name);
    }
    const quint32 cdSize = quint32(out.size()) - cdStart;
    out.append("PK\x05\x06", 4);
    put16(out, 0); put16(out, 0);
    put16(out, quint16(recs.size())); put16(out, quint16(recs.size()));
    put32(out, cdSize); put32(out, cdStart); put16(out, 0);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(out);
    return true;
}

QByteArray floats(const std::vector<float>& v)
{
    QByteArray b;
    b.resize(int(v.size() * 4));
    std::memcpy(b.data(), v.data(), v.size() * 4);
    return b;
}

}  // namespace

TEST(NpzReader, ReadsStoredFloatArraysWithTheirShape)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString p = dir.filePath(QStringLiteral("t.npz"));
    ASSERT_TRUE(writeNpz(p, {
        {"a", npy("<f4", "(2, 3)", floats({1,2,3,4,5,6}))},
        {"b", npy("<f4", "(4,)",   floats({0.5f,-0.5f,2.0f,0.0f}))},
    }));

    NpzReader r;
    QString err;
    ASSERT_TRUE(r.open(p, &err)) << err.toStdString();
    EXPECT_EQ(r.names().size(), 2);
    EXPECT_TRUE(r.contains(QStringLiteral("a")));
    EXPECT_FALSE(r.contains(QStringLiteral("nope")));

    const auto a = r.read(QStringLiteral("a"), &err);
    ASSERT_TRUE(a.valid()) << err.toStdString();
    ASSERT_EQ(a.shape.size(), 2u);
    EXPECT_EQ(a.shape[0], 2);
    EXPECT_EQ(a.shape[1], 3);
    EXPECT_EQ(a.elementCount(), 6);
    ASSERT_EQ(a.data.size(), 6u);
    EXPECT_FLOAT_EQ(a.data[0], 1.0f);
    EXPECT_FLOAT_EQ(a.data[5], 6.0f);

    const auto b = r.read(QStringLiteral("b"), &err);
    ASSERT_TRUE(b.valid());
    EXPECT_FLOAT_EQ(b.data[1], -0.5f);
}

// Reading float64 as float32 yields plausible-looking garbage — a face that
// deforms, but wrongly. It must be refused by name, not reinterpreted.
TEST(NpzReader, NonFloat32DtypeIsRefusedByName)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString p = dir.filePath(QStringLiteral("t.npz"));
    ASSERT_TRUE(writeNpz(p, {
        {"d", npy("<f8", "(2,)", QByteArray(16, '\0'))},
        {"i", npy("<i4", "(2,)", QByteArray(8, '\0'))},
    }));

    NpzReader r;
    ASSERT_TRUE(r.open(p));
    QString err;
    EXPECT_FALSE(r.read(QStringLiteral("d"), &err).valid());
    EXPECT_TRUE(err.contains(QStringLiteral("<f8"))) << "the error must name the dtype: " << err.toStdString();
    EXPECT_FALSE(r.read(QStringLiteral("i"), &err).valid());
}

// Fortran order means the data is column-major; reading it as C order
// transposes the array silently.
TEST(NpzReader, FortranOrderIsRefused)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString p = dir.filePath(QStringLiteral("t.npz"));
    ASSERT_TRUE(writeNpz(p, {
        {"f", npy("<f4", "(2, 2)", floats({1,2,3,4}), /*fortran=*/true)},
    }));
    NpzReader r;
    ASSERT_TRUE(r.open(p));
    QString err;
    EXPECT_FALSE(r.read(QStringLiteral("f"), &err).valid());
    EXPECT_TRUE(err.contains(QStringLiteral("Fortran"))) << err.toStdString();
}

TEST(NpzReader, ReadsFixedWidthStringArrays)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString p = dir.filePath(QStringLiteral("t.npz"));
    // |S8: 8 bytes per entry, NUL-padded — how poseNames is stored.
    QByteArray payload;
    for (const char* s : {"jawOpen", "neutral", "mouthLeft"}) {
        QByteArray e(s);
        e.truncate(8);
        e.append(QByteArray(8 - e.size(), '\0'));
        payload.append(e);
    }
    ASSERT_TRUE(writeNpz(p, {{"poseNames", npy("|S8", "(3,)", payload)}}));

    NpzReader r;
    ASSERT_TRUE(r.open(p));
    QString err;
    const auto names = r.readStrings(QStringLiteral("poseNames"), &err);
    ASSERT_EQ(names.size(), 3) << err.toStdString();
    EXPECT_EQ(names[0], QStringLiteral("jawOpen")) << "trailing NULs must be stripped";
    EXPECT_EQ(names[1], QStringLiteral("neutral"));
}

// The .npy header length is an untrusted quint32. Narrowing it to int made
// the data offset overflow, and at 0x7FFFFFFF the reader handed
// QString::fromLatin1 a huge positive length — an ASan BUS crash, reproduced
// from a crafted ~100-byte archive. A model file is downloaded, so a corrupt
// or hostile one must be refused, not trusted.
TEST(NpzReader, AbsurdHeaderLengthIsRefusedNotOverflowed)
{
    // A version-2 npy (4-byte header length) with a hostile length field.
    auto hostile = [](quint32 hlen) {
        QByteArray out("\x93NUMPY", 6);
        out.append(char(2)); out.append(char(0));     // version 2 -> 4-byte hlen
        put32(out, hlen);
        out.append(QByteArray(16, 'x'));
        return out;
    };

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (quint32 hlen : {0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 100000u}) {
        const QString path = dir.filePath(QStringLiteral("bad%1.npz").arg(hlen));
        ASSERT_TRUE(writeNpz(path, {{"poseNames", hostile(hlen)}}));

        NpzReader r;
        QString err;
        if (!r.open(path, &err)) continue;      // refusing at open is fine too
        EXPECT_TRUE(r.readStrings(QStringLiteral("poseNames"), &err).isEmpty())
            << "hlen " << hlen;                 // must not crash
        EXPECT_FALSE(r.read(QStringLiteral("poseNames"), &err).valid())
            << "hlen " << hlen;
    }
}

// open() is documented all-or-nothing. A malformed entry used to `break` out
// of the central-directory walk, leaving the entries parsed so far in place
// and returning true — a partially-read archive presented as a good one.
TEST(NpzReader, AMalformedCentralDirectoryFailsTheWholeOpen)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("trunc.npz"));
    ASSERT_TRUE(writeNpz(path, {{"a", npy("<f4", "(2,)", floats({1.0f, 2.0f}))},
                                {"b", npy("<f4", "(2,)", floats({3.0f, 4.0f}))}}));

    // Corrupt the SECOND central-directory record's signature. The first
    // entry stays valid, which is exactly the case that used to half-open.
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadWrite));
    QByteArray all = f.readAll();
    const int first = all.indexOf(QByteArray("PK\x01\x02", 4));
    ASSERT_GE(first, 0);
    const int second = all.indexOf(QByteArray("PK\x01\x02", 4), first + 4);
    ASSERT_GE(second, 0) << "the fixture needs two central-directory records";
    all[second + 3] = char(0xFF);
    f.seek(0); f.write(all); f.close();

    NpzReader r;
    QString err;
    EXPECT_FALSE(r.open(path, &err))
        << "a malformed entry must fail the whole open, not half of it";
    EXPECT_FALSE(err.isEmpty());
}

TEST(NpzReader, MissingMemberAndBadFileAreReportedNotCrashed)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    NpzReader missing;
    QString err;
    EXPECT_FALSE(missing.open(dir.filePath(QStringLiteral("nope.npz")), &err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_FALSE(missing.isOpen());

    // A file that exists but is not a zip.
    const QString junk = dir.filePath(QStringLiteral("junk.npz"));
    { QFile f(junk); ASSERT_TRUE(f.open(QIODevice::WriteOnly)); f.write(QByteArray(200, 'x')); }
    NpzReader bad;
    EXPECT_FALSE(bad.open(junk, &err));
    EXPECT_FALSE(err.isEmpty());

    // Valid archive, absent member.
    const QString p = dir.filePath(QStringLiteral("t.npz"));
    ASSERT_TRUE(writeNpz(p, {{"a", npy("<f4", "(1,)", floats({1.0f}))}}));
    NpzReader r;
    ASSERT_TRUE(r.open(p));
    EXPECT_FALSE(r.read(QStringLiteral("absent"), &err).valid());
    EXPECT_TRUE(err.contains(QStringLiteral("absent"))) << err.toStdString();
}
