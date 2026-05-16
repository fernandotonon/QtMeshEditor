#include <gtest/gtest.h>

#include "UpdateVersion.h"

#include <QString>

using UpdateVersion::Comparison;

// ---- normalize ----

TEST(UpdateVersion, NormalizeStripsLeadingVPrefix)
{
    EXPECT_EQ(UpdateVersion::normalize("v3.2.0"), QString("3.2.0"));
    EXPECT_EQ(UpdateVersion::normalize("V3.2.0"), QString("3.2.0"));
    EXPECT_EQ(UpdateVersion::normalize("3.2.0"),  QString("3.2.0"));
}

TEST(UpdateVersion, NormalizeStripsWhitespace)
{
    EXPECT_EQ(UpdateVersion::normalize("  3.2.0  "), QString("3.2.0"));
    EXPECT_EQ(UpdateVersion::normalize("\tv3.2.0\n"), QString("3.2.0"));
}

TEST(UpdateVersion, NormalizeStripsPrereleaseAndBuildSuffixes)
{
    EXPECT_EQ(UpdateVersion::normalize("3.2.0-rc1"),         QString("3.2.0"));
    EXPECT_EQ(UpdateVersion::normalize("3.2.0+build.42"),    QString("3.2.0"));
    EXPECT_EQ(UpdateVersion::normalize("v3.2.0-alpha.1+ci"), QString("3.2.0"));
}

TEST(UpdateVersion, NormalizeRejectsNonNumericInputs)
{
    EXPECT_TRUE(UpdateVersion::normalize("").isEmpty());
    EXPECT_TRUE(UpdateVersion::normalize("latest").isEmpty());
    EXPECT_TRUE(UpdateVersion::normalize("main").isEmpty());
    EXPECT_TRUE(UpdateVersion::normalize("v").isEmpty());
    EXPECT_TRUE(UpdateVersion::normalize("3.x.0").isEmpty());
    // Suffix-only is also rejected because the numeric prefix is empty.
    EXPECT_TRUE(UpdateVersion::normalize("-rc1").isEmpty());
}

TEST(UpdateVersion, NormalizeAcceptsVariableComponentCount)
{
    EXPECT_EQ(UpdateVersion::normalize("3"),       QString("3"));
    EXPECT_EQ(UpdateVersion::normalize("3.2"),     QString("3.2"));
    EXPECT_EQ(UpdateVersion::normalize("3.2.0.5"), QString("3.2.0.5"));
}

// ---- compare ----

TEST(UpdateVersion, CompareEqualWhenStringsMatch)
{
    EXPECT_EQ(UpdateVersion::compare("3.2.0", "3.2.0"), Comparison::Same);
}

TEST(UpdateVersion, CompareEqualWhenVPrefixDiffers)
{
    // Was the root cause of the "permanent update prompt" bug — the
    // string-equality check used to fail here.
    EXPECT_EQ(UpdateVersion::compare("3.2.0", "v3.2.0"), Comparison::Same);
    EXPECT_EQ(UpdateVersion::compare("v3.2.0", "3.2.0"), Comparison::Same);
    EXPECT_EQ(UpdateVersion::compare("V3.2.0", "v3.2.0"), Comparison::Same);
}

TEST(UpdateVersion, CompareNumericallyAcrossComponents)
{
    // String compare would have ranked "3.10.0" < "3.2.0".
    EXPECT_EQ(UpdateVersion::compare("3.2.0", "3.10.0"), Comparison::Older);
    EXPECT_EQ(UpdateVersion::compare("3.10.0", "3.2.0"), Comparison::Newer);
    EXPECT_EQ(UpdateVersion::compare("3.2.0", "4.0.0"),  Comparison::Older);
    EXPECT_EQ(UpdateVersion::compare("3.2.1", "3.2.0"),  Comparison::Newer);
}

TEST(UpdateVersion, CompareIgnoresPrereleaseSuffix)
{
    EXPECT_EQ(UpdateVersion::compare("3.2.0-rc1", "3.2.0"), Comparison::Same);
    EXPECT_EQ(UpdateVersion::compare("3.2.0", "3.2.0-rc1"), Comparison::Same);
}

TEST(UpdateVersion, CompareInvalidWhenAnyInputUnparseable)
{
    EXPECT_EQ(UpdateVersion::compare("", "3.2.0"),       Comparison::Invalid);
    EXPECT_EQ(UpdateVersion::compare("3.2.0", ""),       Comparison::Invalid);
    EXPECT_EQ(UpdateVersion::compare("latest", "3.2.0"), Comparison::Invalid);
    EXPECT_EQ(UpdateVersion::compare("3.2.0", "main"),   Comparison::Invalid);
}

// ---- isUpdateAvailable ----

TEST(UpdateVersion, UpdateAvailableOnlyWhenRemoteStrictlyNewer)
{
    EXPECT_TRUE (UpdateVersion::isUpdateAvailable("3.2.0", "3.3.0"));
    EXPECT_TRUE (UpdateVersion::isUpdateAvailable("3.2.0", "v3.3.0"));
    EXPECT_TRUE (UpdateVersion::isUpdateAvailable("3.2.0", "4.0.0"));
    EXPECT_FALSE(UpdateVersion::isUpdateAvailable("3.2.0", "3.2.0"));
    EXPECT_FALSE(UpdateVersion::isUpdateAvailable("3.2.0", "v3.2.0"));
    EXPECT_FALSE(UpdateVersion::isUpdateAvailable("3.2.0", "3.1.0"));
    // Invalid inputs must NEVER report "update available" — the user
    // would otherwise get a prompt for every garbled API response.
    EXPECT_FALSE(UpdateVersion::isUpdateAvailable("3.2.0", "latest"));
    EXPECT_FALSE(UpdateVersion::isUpdateAvailable("latest", "3.2.0"));
}
