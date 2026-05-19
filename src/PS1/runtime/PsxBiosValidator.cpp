#include "PsxBiosValidator.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

namespace {

struct KnownBios {
    const char *sha1Hex;
    const char *label;
};

// nocash / libretro beetle known retail BIOS SHA-1 values (uppercase hex).
constexpr KnownBios kKnown[] = {
    {"5539CBE6E87414C0CF40CFDE8BC55874C392A39", "SCPH-1001 (USA)"},
    {"0555C6FAE8906F3F09BAF5988F00E55F88E9F30B", "SCPH-5501 (USA)"},
    {"8DD5D87545DAD6BEFE761ED0310EBE40710C39B4", "SCPH-1002 (Europe)"},
    {"F1AC735BBC46205480633DFC45C84566A316D3E", "SCPH-5502 (Europe)"},
    {"529215F293736694AFE84E7499F83E6A730597C", "SCPH-1000 (Japan)"},
    {"96825D7167852ADB8C2CCB61945E7E0806776B19", "SCPH-5500 (Japan)"},
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
