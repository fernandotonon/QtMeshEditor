#ifndef PSXBIOSVALIDATOR_H
#define PSXBIOSVALIDATOR_H

#include <QString>

/** Validates PS1 BIOS images by size and known-good SHA-1 fingerprints. */
class PsxBiosValidator
{
public:
    struct Result {
        bool ok = false;
        QString label;
        QString detail;
    };

    static Result validateFile(const QString &biosPath);
};

#endif // PSXBIOSVALIDATOR_H
