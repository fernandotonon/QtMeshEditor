#include "PsxBiosValidator.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

namespace {

struct KnownBios {
    const char *sha1Hex;
    const char *label;
};

// libretro-database System.dat SHA-1 values (uppercase hex).
constexpr KnownBios kKnown[] = {
    {"10155D8D6E6E832D6EA66DB9BC098321FB5E8EBF", "SCPH-1001 (USA)"},
    {"0555C6FAE8906F3F09BAF5988F00E55F88E9F30B", "SCPH-5501 (USA)"},
    {"20B98F3D80F11CBF5A7BFD0779B0E63760ECC62C", "SCPH-1002 (Europe)"},
    {"F6BC2D1F5EB6593DE7D089C425AC681D6FFFD3F0", "SCPH-5502 (Europe)"},
    {"343883A7B555646DA8CEE54AADD2795B6E7DD070", "SCPH-1000 (Japan)"},
    {"B05DEF971D8EC59F346F2D9AC21FB742E3EB6917", "SCPH-5500 (Japan)"},
};

} // namespace

PsxBiosValidator::Result PsxBiosValidator::validateFile(const QString &biosPath)
{
    Result out;
    const QFileInfo info(biosPath);
    if (!info.exists() || !info.isFile()) {
        out.detail = QObject::tr("BIOS file not found: %1").arg(biosPath);
        return out;
    }

    if (info.size() != 512 * 1024) {
        out.detail = QObject::tr(
            "BIOS must be exactly 512 KiB (524288 bytes); this file is %1 bytes. "
            "Use a verified SCPH-1001 / SCPH-5501 dump.")
            .arg(info.size());
        return out;
    }

    QFile file(biosPath);
    if (!file.open(QIODevice::ReadOnly)) {
        out.detail = QObject::tr("Cannot read BIOS file: %1").arg(biosPath);
        return out;
    }

    QCryptographicHash hash(QCryptographicHash::Sha1);
    if (!hash.addData(&file)) {
        out.detail = QObject::tr("Failed to hash BIOS file: %1").arg(biosPath);
        return out;
    }

    const QString sha1 = QString::fromLatin1(hash.result().toHex()).toUpper();
    for (const KnownBios &known : kKnown) {
        if (sha1 == QLatin1StringView(known.sha1Hex)) {
            out.ok = true;
            out.label = QString::fromLatin1(known.label);
            return out;
        }
    }

    out.detail = QObject::tr(
        "BIOS SHA-1 is not a known retail PlayStation image.\n"
        "File: %1\n"
        "SHA-1: %2\n\n"
        "Expected one of: SCPH-1001 (USA), SCPH-5501 (USA), SCPH-1002 (EU), SCPH-5502 (EU), "
        "SCPH-1000/5500 (Japan).\n"
        "An invalid BIOS will crash the libretro core — replace the file before starting.")
        .arg(info.fileName(), sha1);
    return out;
}
