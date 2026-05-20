#include "PsxBiosValidator.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

namespace {

struct KnownBios {
    const char *sha256Hex;
    const char *sha1Hex;
    const char *label;
};

// libretro-database System.dat SHA-1; SHA-256 from redump / emulator BIOS databases.
constexpr KnownBios kKnown[] = {
    {"1413067D7C1C2789DE5B46FC454FA71B80DF10F6225355D7601AB4DFAC43A1C4",
     "10155D8D6E6E832D6EA66DB9BC098321FB5E8EBF", "SCPH-1001 (USA)"},
    {nullptr, "0555C6FAE8906F3F09BAF5988F00E55F88E9F30B", "SCPH-5501 (USA)"},
    {nullptr, "20B98F3D80F11CBF5A7BFD0779B0E63760ECC62C", "SCPH-1002 (Europe)"},
    {nullptr, "F6BC2D1F5EB6593DE7D089C425AC681D6FFFD3F0", "SCPH-5502 (Europe)"},
    {nullptr, "343883A7B555646DA8CEE54AADD2795B6E7DD070", "SCPH-1000 (Japan)"},
    {nullptr, "B05DEF971D8EC59F346F2D9AC21FB742E3EB6917", "SCPH-5500 (Japan)"},
};

QString lookupLabel(const QString &sha256, const QString &sha1)
{
    for (const KnownBios &known : kKnown) {
        if (known.sha256Hex && sha256 == QLatin1StringView(known.sha256Hex))
            return QString::fromLatin1(known.label);
    }
    for (const KnownBios &known : kKnown) {
        if (sha1 == QLatin1StringView(known.sha1Hex))
            return QString::fromLatin1(known.label);
    }
    return {};
}

} // namespace

PsxBiosValidator::Fingerprint PsxBiosValidator::fingerprintFile(const QString &biosPath)
{
    Fingerprint out;
    const QFileInfo info(biosPath);
    if (!info.exists() || !info.isFile())
        return out;

    out.readable = true;
    out.sizeOk = info.size() == 512 * 1024;
    if (!out.sizeOk)
        return out;

    QFile file(biosPath);
    if (!file.open(QIODevice::ReadOnly))
        return out;

    const QByteArray data = file.readAll();
    if (data.size() != 512 * 1024)
        return out;

    QCryptographicHash sha256(QCryptographicHash::Sha256);
    sha256.addData(data);
    out.sha256Hex = QString::fromLatin1(sha256.result().toHex()).toUpper();

    QCryptographicHash sha1(QCryptographicHash::Sha1);
    sha1.addData(data);
    out.sha1Hex = QString::fromLatin1(sha1.result().toHex()).toUpper();

    out.knownLabel = lookupLabel(out.sha256Hex, out.sha1Hex);
    return out;
}

PsxBiosValidator::Result PsxBiosValidator::validateFile(const QString &biosPath)
{
    Result out;
    const Fingerprint fp = fingerprintFile(biosPath);
    if (!fp.readable) {
        out.detail = QObject::tr("BIOS file not found: %1").arg(biosPath);
        return out;
    }
    if (!fp.sizeOk) {
        out.detail = QObject::tr(
            "BIOS must be exactly 512 KiB (524288 bytes). Use a verified SCPH-1001 / SCPH-5501 dump.");
        return out;
    }

    out.ok = true;
    out.label = fp.knownLabel;
    if (fp.knownLabel.isEmpty()) {
        out.detail = QObject::tr("Unknown BIOS fingerprint (SHA-256: %1).").arg(fp.sha256Hex);
    } else {
        out.detail = fp.knownLabel;
    }
    return out;
}
