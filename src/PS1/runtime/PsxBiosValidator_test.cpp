#ifdef ENABLE_PS1_RIP
#include <gtest/gtest.h>

#include "PS1/runtime/PsxBiosValidator.h"

#include <QTemporaryFile>

TEST(PsxBiosValidatorTest, RejectsWrongSize)
{
    QTemporaryFile file;
    ASSERT_TRUE(file.open());
    file.write(QByteArray(1024, '\0'));
    file.close();

    const auto fp = PsxBiosValidator::fingerprintFile(file.fileName());
    EXPECT_FALSE(fp.sizeOk);
    const auto result = PsxBiosValidator::validateFile(file.fileName());
    EXPECT_FALSE(result.ok);
}

TEST(PsxBiosValidatorTest, UnknownSha256StillLoadsWhenSizeOk)
{
    QTemporaryFile file;
    ASSERT_TRUE(file.open());
    file.write(QByteArray(512 * 1024, '\0'));
    file.close();

    const auto fp = PsxBiosValidator::fingerprintFile(file.fileName());
    EXPECT_TRUE(fp.sizeOk);
    EXPECT_EQ(fp.sha256Hex.size(), 64);
    EXPECT_TRUE(fp.knownLabel.isEmpty());

    const auto result = PsxBiosValidator::validateFile(file.fileName());
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.detail.contains(QStringLiteral("SHA-256")));
}
#endif
