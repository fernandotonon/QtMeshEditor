#ifndef CHARACTERGEN_QMCBUNDLE_H
#define CHARACTERGEN_QMCBUNDLE_H

// Character generator: reader for the "QMCB1" asset container written by scripts/build-character-assets.py.
//
// A flat list of named, typed, n-dimensional blobs (f32 / f16 / i32 / u8). Deliberately tiny: the generator ships two files
// (the MHR body and the ICT head) and needs fp16 for the 100 identity modes + 51 expression shapes, which NpzReader (f32 only)
// cannot read. Pure data, Qt file I/O only — testable against a synthetic buffer.

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

namespace CharacterGen {

class QmcBundle {
public:
    enum class DType : uint8_t { F32 = 0, F16 = 1, I32 = 2, U8 = 3 };

    struct Blob {
        DType type = DType::F32;
        std::vector<uint64_t> shape;
        QByteArray bytes;
        uint64_t elementCount() const;
    };

    /// Parse a whole archive from memory. Returns false and sets `error` on anything malformed (bad magic, truncated blob,
    /// byte length that does not match shape * element size); no partial state is kept.
    bool parse(const QByteArray& data, QString* error = nullptr);
    bool open(const QString& path, QString* error = nullptr);

    bool contains(const QString& name) const { return m_blobs.contains(name); }
    QStringList names() const { return m_blobs.keys(); }
    const Blob* find(const QString& name) const;

    /// Typed reads. Return an empty vector (and set `error`) for a missing name or an incompatible dtype. f16 is widened to f32.
    std::vector<float> readFloat(const QString& name, QString* error = nullptr) const;
    std::vector<int32_t> readInt(const QString& name, QString* error = nullptr) const;
    QByteArray readBytes(const QString& name, QString* error = nullptr) const;
    std::vector<uint64_t> shapeOf(const QString& name) const;

    /// IEEE half -> float (public so the writer-side tests can round-trip it).
    static float halfToFloat(uint16_t h);

private:
    QHash<QString, Blob> m_blobs;
};

}  // namespace CharacterGen

#endif  // CHARACTERGEN_QMCBUNDLE_H
