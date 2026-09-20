#ifndef AUDIOTOFACE_NPZREADER_H
#define AUDIOTOFACE_NPZREADER_H

// Audio2Face (#1019): read the float arrays out of NumPy `.npz` archives.
//
// NVIDIA ships the PCA basis and the ARKit blendshape deltas as `.npz` — a
// plain zip of `.npy` members. Two properties make this cheap to support
// without adding a dependency:
//
//   * both shipped archives store their members UNCOMPRESSED (verified:
//     compress_type 0 on every member of model_data.npz and bs_skin.npz), so
//     no zlib/miniz is needed — the payload is read straight out of the file;
//   * `.npy` is a 6-byte magic, a 2-byte version, a length-prefixed ASCII
//     dict header, then raw little-endian data.
//
// A DEFLATED member is detected and reported rather than silently skipped,
// since a future re-upload could change that and a missing array would
// otherwise present as a broken model.
//
// Only what A2F needs is supported: little-endian float32, C-contiguous. Any
// other dtype is refused by name rather than reinterpreted — reading float64
// as float32 yields plausible-looking garbage.
//
// Qt is used for file I/O only (QFile/QByteArray); there is no Ogre and no
// ONNX here, so this is testable against a synthetic archive.

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

namespace AudioToFace {

struct NpyArray {
    std::vector<int64_t> shape;
    std::vector<float> data;     ///< flattened, C order

    bool valid() const { return !data.empty() && !shape.empty(); }
    /// Product of the shape; equals data.size() for a well-formed array.
    int64_t elementCount() const;
};

class NpzReader {
public:
    /// Opens and indexes the archive. Returns false and sets `error` on a
    /// malformed file; no partial state is left behind.
    bool open(const QString& path, QString* error = nullptr);

    bool isOpen() const { return m_open; }
    /// Member names WITHOUT the `.npy` suffix, as NumPy presents them.
    QStringList names() const;
    bool contains(const QString& name) const;

    /// Read one member as float32. Returns an invalid array (and sets
    /// `error`) for a missing name, a non-float32 dtype, a Fortran-order
    /// array, or a compressed member.
    NpyArray read(const QString& name, QString* error = nullptr) const;

    /// Read a member of fixed-width byte strings (NumPy `|Sn`), used for
    /// the pose-name table in bs_skin.npz. Trailing NULs are stripped.
    QStringList readStrings(const QString& name, QString* error = nullptr) const;

private:
    struct Entry {
        QString name;
        qint64 dataOffset = 0;   ///< absolute offset of the .npy payload
        qint64 size = 0;         ///< uncompressed size
        bool deflated = false;
    };
    bool m_open = false;
    QString m_path;
    std::vector<Entry> m_entries;

    const Entry* find(const QString& name) const;
    QByteArray readRaw(const Entry& e, QString* error) const;
};

}  // namespace AudioToFace

#endif  // AUDIOTOFACE_NPZREADER_H
