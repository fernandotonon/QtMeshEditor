#ifndef MINISIGNVERIFY_H
#define MINISIGNVERIFY_H

#include <QString>

/**
 * @brief Verify minisign (.minisig) detached signatures on release artifacts.
 *
 * Spike PoC for epic #439 / issue #440. Uses the same pre-hashed Ed25519
 * format as the minisign CLI (BLAKE2b-512 digest + dual signatures). The
 * production public key is compiled in; CI signs artifacts with the matching
 * secret (see docs/AUTO_UPDATER_DESIGN.md).
 *
 * Linux builds link libsodium for verify. Other platforms return
 * @ref Result::Unsupported until #445 lands cross-platform crypto.
 */
namespace MinisignVerify {

enum class Result {
    Ok,
    Unsupported,       ///< Platform build without verify backend
    IoError,
    InvalidPublicKey,
    InvalidSignatureFile,
    KeyIdMismatch,
    FileSignatureFailed,
    CommentSignatureFailed,
};

struct Outcome {
    Result result = Result::IoError;
    QString trustedComment;
    QString errorMessage;
};

/// Human-readable error for logging / UI.
QString resultToString(Result result);

/// Base64 public key body for release artifact verification (`minisign -P` form).
QString productionPublicKeyBase64();

/**
 * @brief Verify @p messageFile against @p signatureFile using the production pubkey.
 */
Outcome verifyReleaseFile(const QString& messageFile, const QString& signatureFile);

/**
 * @brief Verify @p messageFile against @p signatureFile using an embedded pubkey.
 *
 * @p publicKeyBase64 is the raw base64 body from a minisign .pub file (the
 * line after the untrusted comment), or the `-P` string minisign prints after
 * key generation.
 */
Outcome verifyFile(const QString& messageFile,
                   const QString& signatureFile,
                   const QString& publicKeyBase64);

} // namespace MinisignVerify

#endif // MINISIGNVERIFY_H
