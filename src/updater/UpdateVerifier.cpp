#include "UpdateVerifier.h"

#include <QCryptographicHash>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

namespace UpdateVerifier {

namespace {

QString readManifestHashForFile(const QString& manifestPath, const QString& fileName, QString* error)
{
    QFile manifest(manifestPath);
    if (!manifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("Cannot read SHA256SUMS manifest");
        }
        return {};
    }

    QTextStream stream(&manifest);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const int spaceIdx = line.indexOf(QStringLiteral("  "));
        if (spaceIdx <= 0) {
            continue;
        }
        const QString hash = line.left(spaceIdx).trimmed();
        const QString name = line.mid(spaceIdx + 2).trimmed();
        if (name.compare(fileName, Qt::CaseInsensitive) == 0) {
            return hash;
        }
    }

    if (error) {
        *error = QStringLiteral("SHA256SUMS does not list %1").arg(fileName);
    }
    return {};
}

} // namespace

QString sha256HexOfFile(const QString& filePath, QString* errorMessage)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot read file for SHA-256: %1").arg(filePath);
        }
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("SHA-256 read failed: %1").arg(filePath);
        }
        return {};
    }

    return QString::fromLatin1(hash.result().toHex());
}

bool verifySha256Hex(const QString& filePath,
                     const QString& expectedHex,
                     QString* errorMessage)
{
    const QString actual = sha256HexOfFile(filePath, errorMessage);
    if (actual.isEmpty()) {
        return false;
    }
    if (actual.compare(expectedHex.trimmed(), Qt::CaseInsensitive) != 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("SHA-256 mismatch for %1").arg(filePath);
        }
        return false;
    }
    return true;
}

Outcome verifySha256FromManifest(const QString& filePath,
                                 const QString& manifestPath,
                                 const QString& fileName)
{
    Outcome out;
    QString lookupError;
    const QString expected = readManifestHashForFile(manifestPath, fileName, &lookupError);
    if (expected.isEmpty()) {
        out.failedStage = QStringLiteral("sha256");
        out.errorMessage = lookupError;
        return out;
    }

    QString verifyError;
    if (!verifySha256Hex(filePath, expected, &verifyError)) {
        out.failedStage = QStringLiteral("sha256");
        out.errorMessage = verifyError;
        return out;
    }

    out.ok = true;
    return out;
}

Outcome verifyDownloadedArtifact(const QString& artifactPath,
                                 const QString& signaturePath,
                                 const QString& manifestPath,
                                 const QString& fileName)
{
    Outcome out;

    if (!manifestPath.isEmpty()) {
        const Outcome shaOutcome =
            verifySha256FromManifest(artifactPath, manifestPath, fileName);
        if (!shaOutcome.ok) {
            return shaOutcome;
        }
    }

#if defined(QTMESH_MINISIGN_VERIFY) && QTMESH_MINISIGN_VERIFY
    const MinisignVerify::Outcome sigOutcome =
        MinisignVerify::verifyReleaseFile(artifactPath, signaturePath);
    if (sigOutcome.result != MinisignVerify::Result::Ok) {
        out.failedStage = QStringLiteral("minisign");
        out.errorMessage = sigOutcome.errorMessage.isEmpty()
            ? MinisignVerify::resultToString(sigOutcome.result)
            : sigOutcome.errorMessage;
        return out;
    }
#else
    Q_UNUSED(signaturePath);
    out.failedStage = QStringLiteral("minisign");
    out.errorMessage = QStringLiteral("Signature verification is not available in this build");
    return out;
#endif

    out.ok = true;
    return out;
}

} // namespace UpdateVerifier
