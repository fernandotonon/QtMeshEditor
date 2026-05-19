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

    const auto result = PsxBiosValidator::validateFile(file.fileName());
    EXPECT_FALSE(result.ok);
}

TEST(PsxBiosValidatorTest, RejectsUnknownSha1)
{
    QTemporaryFile file;
    ASSERT_TRUE(file.open());
    file.write(QByteArray(512 * 1024, '\0'));
    file.close();

    const auto result = PsxBiosValidator::validateFile(file.fileName());
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.detail.contains(QStringLiteral("SHA-1")));
}
#endif
