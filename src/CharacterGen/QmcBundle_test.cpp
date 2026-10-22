#include <gtest/gtest.h>

#include "QmcBundle.h"

#include <cstring>

using namespace CharacterGen;

namespace {
void putBlob(QByteArray& out, const char* name, uint8_t dtype, std::vector<uint64_t> shape, const QByteArray& raw)
{
    const uint32_t nl = uint32_t(std::strlen(name)), nd = uint32_t(shape.size());
    out.append(reinterpret_cast<const char*>(&nl), 4);
    out.append(name, int(nl));
    out.append(char(dtype));
    out.append(reinterpret_cast<const char*>(&nd), 4);
    for (uint64_t d : shape) out.append(reinterpret_cast<const char*>(&d), 8);
    const uint64_t bl = uint64_t(raw.size());
    out.append(reinterpret_cast<const char*>(&bl), 8);
    out.append(raw);
}
QByteArray header(uint32_t count)
{
    QByteArray b("QMCB1\0\0\0", 8);
    b.append(reinterpret_cast<const char*>(&count), 4);
    return b;
}
}  // namespace

TEST(QmcBundle, ReadsEveryDtypeAndWidensHalfFloats)
{
    QByteArray b = header(4);
    const float f[3] = {1.5f, -2.0f, 0.25f};
    putBlob(b, "f", 0, {3}, QByteArray(reinterpret_cast<const char*>(f), 12));
    const uint16_t h[2] = {0x3C00, 0xC000};   // 1.0, -2.0
    putBlob(b, "h", 1, {2}, QByteArray(reinterpret_cast<const char*>(h), 4));
    const int32_t i[2] = {-7, 42};
    putBlob(b, "i", 2, {2}, QByteArray(reinterpret_cast<const char*>(i), 8));
    putBlob(b, "u", 3, {2}, QByteArray("hi", 2));
    QmcBundle q;
    QString err;
    ASSERT_TRUE(q.parse(b, &err)) << err.toStdString();
    EXPECT_EQ(q.readFloat("f"), (std::vector<float>{1.5f, -2.0f, 0.25f}));
    EXPECT_EQ(q.readFloat("h"), (std::vector<float>{1.0f, -2.0f}));
    EXPECT_EQ(q.readInt("i"), (std::vector<int32_t>{-7, 42}));
    EXPECT_EQ(q.readBytes("u"), QByteArray("hi"));
    EXPECT_EQ(q.shapeOf("f"), (std::vector<uint64_t>{3}));
}

TEST(QmcBundle, RefusesAWrongTypeAMissingNameAndBadInput)
{
    QByteArray b = header(1);
    const int32_t i[1] = {1};
    putBlob(b, "i", 2, {1}, QByteArray(reinterpret_cast<const char*>(i), 4));
    QmcBundle q;
    ASSERT_TRUE(q.parse(b));
    QString err;
    EXPECT_TRUE(q.readFloat("i", &err).empty());          // an int array is not silently reinterpreted as floats
    EXPECT_FALSE(err.isEmpty());
    EXPECT_TRUE(q.readInt("missing", &err).empty());
    EXPECT_FALSE(q.parse(QByteArray("NOPE0000", 8), &err));
    EXPECT_FALSE(q.parse(b.left(b.size() - 2), &err));    // truncated blob
    EXPECT_FALSE(q.contains("i")) << "a failed parse must leave no partial state";
}

TEST(QmcBundle, RejectsAByteLengthThatDoesNotMatchTheShape)
{
    QByteArray b = header(1);
    putBlob(b, "f", 0, {4}, QByteArray(8, '\0'));          // 4 floats declared, 2 stored
    QmcBundle q;
    QString err;
    EXPECT_FALSE(q.parse(b, &err));
    EXPECT_NE(err.indexOf("does not match"), -1);
}

TEST(QmcBundle, HalfFloatConversionCoversSubnormalsAndSpecials)
{
    EXPECT_FLOAT_EQ(QmcBundle::halfToFloat(0x0000), 0.0f);
    EXPECT_FLOAT_EQ(QmcBundle::halfToFloat(0x3555), 0.333251953125f);
    EXPECT_FLOAT_EQ(QmcBundle::halfToFloat(0x0001), 5.9604645e-08f);     // smallest subnormal
    EXPECT_TRUE(std::isinf(QmcBundle::halfToFloat(0x7C00)));
    EXPECT_TRUE(std::isnan(QmcBundle::halfToFloat(0x7E00)));
}
