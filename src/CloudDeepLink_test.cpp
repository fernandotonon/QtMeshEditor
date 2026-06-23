#include <gtest/gtest.h>

#include "CloudDeepLink.h"

TEST(CloudDeepLinkTest, ParsesCloudOpenUrl)
{
    CloudDeepLinkTarget target;
    ASSERT_TRUE(CloudDeepLink::parseUrl(
        QStringLiteral("qtmesh://cloud/open?owner=fernandotonon&project=qtmesheditor"), &target));
    EXPECT_EQ(target.ownerSlug, QStringLiteral("fernandotonon"));
    EXPECT_EQ(target.projectSlug, QStringLiteral("qtmesheditor"));
}

TEST(CloudDeepLinkTest, RoundTripsLaunchToken)
{
    const QString token =
        CloudDeepLink::encodeLaunchToken(QStringLiteral("me"), QStringLiteral("hero"));
    CloudDeepLinkTarget target;
    ASSERT_TRUE(CloudDeepLink::decodeLaunchToken(token, &target));
    EXPECT_EQ(target.ownerSlug, QStringLiteral("me"));
    EXPECT_EQ(target.projectSlug, QStringLiteral("hero"));
}
