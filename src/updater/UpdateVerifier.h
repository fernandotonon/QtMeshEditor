#ifndef UPDATEVERIFIER_H
#define UPDATEVERIFIER_H

#include "MinisignVerify.h"

#include <QString>

/**
 * @brief SHA-256 + minisign verification for downloaded update artifacts (#445).
 */
namespace UpdateVerifier {

struct Outcome {
    bool ok = false;
    QString errorMessage;
    /// @c sha256, @c minisign, or empty on success.
    QString failedStage;
};

/// Compute lowercase hex SHA-256 of @p filePath.
QString sha256HexOfFile(const QString& filePath, QString* errorMessage = nullptr);

/// Compare @p filePath digest to @p expectedHex (case-insensitive).
bool verifySha256Hex(const QString& filePath,
                     const QString& expectedHex,
                     QString* errorMessage = nullptr);

/**
 * @brief Look up @p fileName in a GNU @c sha256sum manifest and verify the file.
 *
 * Manifest lines: @c "<hex>  <filename>" (two spaces before name, per CI).
 */
Outcome verifySha256FromManifest(const QString& filePath,
                                 const QString& manifestPath,
                                 const QString& fileName);

/**
 * @brief Full verify pipeline: optional SHA256SUMS entry + minisign signature.
 *
 * When @p manifestPath is empty, SHA-256 manifest check is skipped. Minisign
 * verification is always required when compiled with @c QTMESH_MINISIGN_VERIFY.
 */
Outcome verifyDownloadedArtifact(const QString& artifactPath,
                                 const QString& signaturePath,
                                 const QString& manifestPath,
                                 const QString& fileName);

} // namespace UpdateVerifier

#endif // UPDATEVERIFIER_H
