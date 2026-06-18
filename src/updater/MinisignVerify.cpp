#include "MinisignVerify.h"

#include <QByteArray>
#include <QFile>
#include <QTextStream>

#if defined(QTMESH_MINISIGN_VERIFY) && QTMESH_MINISIGN_VERIFY
#include <sodium.h>
#endif

namespace MinisignVerify {

namespace {

// packaging/updater/minisign.pub — rotate via docs/AUTO_UPDATER_DESIGN.md §1.
constexpr const char kProductionPublicKeyBase64[] =
    "RWQYgVXaH+8eYME58t9l6roX1QIqFsW+/nYV216ymPtW8H6odA8aMGhJ";

} // namespace

constexpr int kSigAlgBytes = 2;
constexpr int kKeyNumBytes = 8;
constexpr int kSigBytes = 64;
constexpr int kPubKeyBytes = 32;
constexpr int kSigStructBytes = kSigAlgBytes + kKeyNumBytes + kSigBytes;
constexpr int kPubStructBytes = kSigAlgBytes + kKeyNumBytes + kPubKeyBytes;

Outcome fail(Result result, const QString& message)
{
    Outcome out;
    out.result = result;
    out.errorMessage = message;
    return out;
}

QByteArray decodeBase64(const QString& encoded)
{
    const QByteArray trimmed = encoded.trimmed().toLatin1();
    return QByteArray::fromBase64(trimmed);
}

#if defined(QTMESH_MINISIGN_VERIFY) && QTMESH_MINISIGN_VERIFY

bool ensureSodiumInit()
{
    static bool ready = [] {
        return sodium_init() >= 0;
    }();
    return ready;
}

Outcome verifyParsed(const QByteArray& message,
                     const QByteArray& sigStruct,
                     const QByteArray& globalSig,
                     const QString& trustedComment,
                     const QByteArray& pubStruct)
{
    if (!ensureSodiumInit())
        return fail(Result::Unsupported, QStringLiteral("libsodium init failed"));

    if (sigStruct.size() < kSigStructBytes || pubStruct.size() < kPubStructBytes)
        return fail(Result::InvalidSignatureFile, QStringLiteral("signature or pubkey too short"));

    if (sigStruct.mid(kSigAlgBytes, kKeyNumBytes) != pubStruct.mid(kSigAlgBytes, kKeyNumBytes))
        return fail(Result::KeyIdMismatch, QStringLiteral("signature key id does not match public key"));

    const auto sigAlg = sigStruct.left(kSigAlgBytes);
    const bool hashed = sigAlg == QByteArray("ED", 2);
    if (!hashed && sigAlg != QByteArray("Ed", 2))
        return fail(Result::InvalidSignatureFile, QStringLiteral("unsupported signature algorithm"));

    const QByteArray detachedSig = sigStruct.mid(kSigAlgBytes + kKeyNumBytes, kSigBytes);
    const QByteArray publicKey = pubStruct.mid(kSigAlgBytes + kKeyNumBytes, kPubKeyBytes);

    QByteArray payload = message;
    if (hashed) {
        payload.resize(crypto_generichash_BYTES_MAX);
        if (crypto_generichash(reinterpret_cast<unsigned char*>(payload.data()),
                               crypto_generichash_BYTES_MAX,
                               reinterpret_cast<const unsigned char*>(message.constData()),
                               static_cast<unsigned long long>(message.size()),
                               nullptr,
                               0) != 0)
            return fail(Result::IoError, QStringLiteral("BLAKE2b digest failed"));
    }

    if (crypto_sign_verify_detached(reinterpret_cast<const unsigned char*>(detachedSig.constData()),
                                    reinterpret_cast<const unsigned char*>(payload.constData()),
                                    payload.size(),
                                    reinterpret_cast<const unsigned char*>(publicKey.constData())) != 0)
        return fail(Result::FileSignatureFailed, QStringLiteral("file signature invalid"));

    const QByteArray trustedUtf8 = trustedComment.toUtf8();
    QByteArray commentPayload;
    commentPayload.reserve(kSigBytes + trustedUtf8.size());
    commentPayload.append(detachedSig);
    commentPayload.append(trustedUtf8);

    if (globalSig.size() < kSigBytes
        || crypto_sign_verify_detached(reinterpret_cast<const unsigned char*>(globalSig.constData()),
                                       reinterpret_cast<const unsigned char*>(commentPayload.constData()),
                                       commentPayload.size(),
                                       reinterpret_cast<const unsigned char*>(publicKey.constData())) != 0)
        return fail(Result::CommentSignatureFailed, QStringLiteral("trusted comment signature invalid"));

    Outcome ok;
    ok.result = Result::Ok;
    ok.trustedComment = trustedComment;
    return ok;
}

#endif // QTMESH_MINISIGN_VERIFY

QString resultToString(Result result)
{
    switch (result) {
    case Result::Ok: return QStringLiteral("ok");
    case Result::Unsupported: return QStringLiteral("unsupported");
    case Result::IoError: return QStringLiteral("io_error");
    case Result::InvalidPublicKey: return QStringLiteral("invalid_public_key");
    case Result::InvalidSignatureFile: return QStringLiteral("invalid_signature_file");
    case Result::KeyIdMismatch: return QStringLiteral("key_id_mismatch");
    case Result::FileSignatureFailed: return QStringLiteral("file_signature_failed");
    case Result::CommentSignatureFailed: return QStringLiteral("comment_signature_failed");
    }
    return QStringLiteral("unknown");
}

QString productionPublicKeyBase64()
{
    return QString::fromUtf8(kProductionPublicKeyBase64);
}

Outcome verifyReleaseFile(const QString& messageFile, const QString& signatureFile)
{
    return verifyFile(messageFile, signatureFile, productionPublicKeyBase64());
}

Outcome verifyFile(const QString& messageFile,
                   const QString& signatureFile,
                   const QString& publicKeyBase64)
{
#if !defined(QTMESH_MINISIGN_VERIFY) || !QTMESH_MINISIGN_VERIFY
    Q_UNUSED(messageFile);
    Q_UNUSED(signatureFile);
    Q_UNUSED(publicKeyBase64);
    return fail(Result::Unsupported,
                QStringLiteral("minisign verify is not available in this build"));
#else
    const QByteArray pubStruct = decodeBase64(publicKeyBase64);
    if (pubStruct.size() < kPubStructBytes)
        return fail(Result::InvalidPublicKey, QStringLiteral("could not decode public key"));

    QFile messageFp(messageFile);
    if (!messageFp.open(QIODevice::ReadOnly))
        return fail(Result::IoError, QStringLiteral("cannot read message file"));

    QFile sigFp(signatureFile);
    if (!sigFp.open(QIODevice::ReadOnly | QIODevice::Text))
        return fail(Result::IoError, QStringLiteral("cannot read signature file"));

    QTextStream sigStream(&sigFp);
    const QString untrustedLine = sigStream.readLine();
    const QString sigLine = sigStream.readLine();
    const QString trustedLine = sigStream.readLine();
    const QString globalLine = sigStream.readLine();

    if (untrustedLine.isEmpty() || sigLine.isEmpty() || trustedLine.isEmpty() || globalLine.isEmpty())
        return fail(Result::InvalidSignatureFile, QStringLiteral("signature file is incomplete"));

    if (!untrustedLine.startsWith(QStringLiteral("untrusted comment:")))
        return fail(Result::InvalidSignatureFile, QStringLiteral("missing untrusted comment line"));

    if (!trustedLine.startsWith(QStringLiteral("trusted comment:")))
        return fail(Result::InvalidSignatureFile, QStringLiteral("missing trusted comment line"));

    const QString trustedComment =
        trustedLine.mid(QStringLiteral("trusted comment:").size()).trimmed();

    const QByteArray sigStruct = decodeBase64(sigLine);
    const QByteArray globalSig = decodeBase64(globalLine);
    const QByteArray message = messageFp.readAll();

    return verifyParsed(message, sigStruct, globalSig, trustedComment, pubStruct);
#endif
}

} // namespace MinisignVerify
