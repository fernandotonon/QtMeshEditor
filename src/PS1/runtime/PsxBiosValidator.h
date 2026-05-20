#ifndef PSXBIOSVALIDATOR_H
#define PSXBIOSVALIDATOR_H

#include <QString>

/** PS1 BIOS size check + SHA-256/SHA-1 fingerprinting (#417). Never blocks on unknown hash. */
class PsxBiosValidator
{
public:
    struct Fingerprint {
        bool readable = false;
        bool sizeOk = false;
        QString sha256Hex;
        QString sha1Hex;
        QString knownLabel;
    };

    static Fingerprint fingerprintFile(const QString &biosPath);

    /** @deprecated Use fingerprintFile; kept for callers that only need pass/fail on size. */
    struct Result {
        bool ok = false;
        QString label;
        QString detail;
    };

    static Result validateFile(const QString &biosPath);
};

#endif // PSXBIOSVALIDATOR_H
