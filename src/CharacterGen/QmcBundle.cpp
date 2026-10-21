#include "QmcBundle.h"

#include <QFile>

#include <cmath>
#include <cstring>

namespace CharacterGen {

namespace {
size_t elementSize(QmcBundle::DType t)
{
    switch (t) {
        case QmcBundle::DType::F32: case QmcBundle::DType::I32: return 4;
        case QmcBundle::DType::F16: return 2;
        case QmcBundle::DType::U8: return 1;
    }
    return 0;
}

struct Cursor {
    const char* p;
    const char* end;
    bool take(void* out, size_t n)
    {
        if (size_t(end - p) < n) return false;
        std::memcpy(out, p, n);
        p += n;
        return true;
    }
};
}  // namespace

uint64_t QmcBundle::Blob::elementCount() const
{
    uint64_t n = shape.empty() ? 0 : 1;
    for (uint64_t d : shape) n *= d;
    return n;
}

float QmcBundle::halfToFloat(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu;
    float v;
    if (exp == 0) v = std::ldexp(float(man), -24);                               // zero / subnormal
    else if (exp == 31) v = man ? std::nanf("") : INFINITY;
    else v = std::ldexp(float(man | 0x400u), int(exp) - 25);
    return sign ? -v : v;
}

bool QmcBundle::parse(const QByteArray& data, QString* error)
{
    auto fail = [&](const QString& msg) {
        if (error) *error = msg;
        m_blobs.clear();
        return false;
    };
    m_blobs.clear();
    Cursor c{data.constData(), data.constData() + data.size()};
    char magic[8];
    if (!c.take(magic, 8) || std::memcmp(magic, "QMCB1\0\0\0", 8) != 0) return fail(QStringLiteral("not a QMCB1 bundle"));
    uint32_t count = 0;
    if (!c.take(&count, 4) || count > 4096) return fail(QStringLiteral("bad blob count"));
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t nameLen = 0;
        if (!c.take(&nameLen, 4) || nameLen == 0 || nameLen > 256 || size_t(c.end - c.p) < nameLen)
            return fail(QStringLiteral("truncated blob name"));
        const QString name = QString::fromUtf8(c.p, int(nameLen));
        c.p += nameLen;
        uint8_t dtype = 0;
        uint32_t ndim = 0;
        if (!c.take(&dtype, 1) || dtype > 3 || !c.take(&ndim, 4) || ndim == 0 || ndim > 8)
            return fail(QStringLiteral("bad header for '%1'").arg(name));
        Blob b;
        b.type = DType(dtype);
        uint64_t elements = 1;
        for (uint32_t d = 0; d < ndim; ++d) {
            uint64_t dim = 0;
            if (!c.take(&dim, 8) || dim > (1ull << 40)) return fail(QStringLiteral("bad shape for '%1'").arg(name));
            b.shape.push_back(dim);
            if (dim != 0 && elements > (1ull << 40) / dim) return fail(QStringLiteral("shape of '%1' is too large").arg(name));
            elements *= dim;
        }
        uint64_t byteLen = 0;
        if (!c.take(&byteLen, 8) || byteLen != elements * elementSize(b.type) || uint64_t(c.end - c.p) < byteLen)
            return fail(QStringLiteral("size of '%1' does not match its shape").arg(name));
        b.bytes = QByteArray(c.p, int(byteLen));
        c.p += byteLen;
        m_blobs.insert(name, std::move(b));
    }
    return true;
}

bool QmcBundle::open(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot open %1").arg(path);
        return false;
    }
    return parse(f.readAll(), error);
}

const QmcBundle::Blob* QmcBundle::find(const QString& name) const
{
    auto it = m_blobs.constFind(name);
    return it == m_blobs.constEnd() ? nullptr : &it.value();
}

std::vector<uint64_t> QmcBundle::shapeOf(const QString& name) const
{
    const Blob* b = find(name);
    return b ? b->shape : std::vector<uint64_t>{};
}

std::vector<float> QmcBundle::readFloat(const QString& name, QString* error) const
{
    const Blob* b = find(name);
    if (!b || (b->type != DType::F32 && b->type != DType::F16)) {
        if (error) *error = b ? QStringLiteral("'%1' is not a float array").arg(name) : QStringLiteral("missing '%1'").arg(name);
        return {};
    }
    const uint64_t n = b->elementCount();
    std::vector<float> out(n);
    if (b->type == DType::F32) {
        std::memcpy(out.data(), b->bytes.constData(), n * 4);
    } else {
        const char* src = b->bytes.constData();
        for (uint64_t i = 0; i < n; ++i) {
            uint16_t h;
            std::memcpy(&h, src + i * 2, 2);
            out[i] = halfToFloat(h);
        }
    }
    return out;
}

std::vector<int32_t> QmcBundle::readInt(const QString& name, QString* error) const
{
    const Blob* b = find(name);
    if (!b || b->type != DType::I32) {
        if (error) *error = b ? QStringLiteral("'%1' is not an int array").arg(name) : QStringLiteral("missing '%1'").arg(name);
        return {};
    }
    std::vector<int32_t> out(b->elementCount());
    std::memcpy(out.data(), b->bytes.constData(), out.size() * 4);
    return out;
}

QByteArray QmcBundle::readBytes(const QString& name, QString* error) const
{
    const Blob* b = find(name);
    if (!b || b->type != DType::U8) {
        if (error) *error = b ? QStringLiteral("'%1' is not a byte array").arg(name) : QStringLiteral("missing '%1'").arg(name);
        return {};
    }
    return b->bytes;
}

}  // namespace CharacterGen
