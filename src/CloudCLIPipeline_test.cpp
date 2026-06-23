#include "CloudCLIPipeline.h"
#include "CloudCredentialStore.h"

#include <gtest/gtest.h>

#include <QCoreApplication>

namespace {

int runCloud(int argc, char** argv)
{
    return CloudCLIPipeline::run(argc, argv);
}

} // namespace

TEST(CloudCLIPipeline, UnknownSubcommandReturnsUsageError)
{
    char arg0[] = "qtmesh";
    char arg1[] = "cloud";
    char arg2[] = "nope";
    char* argv[] = {arg0, arg1, arg2};
    EXPECT_EQ(runCloud(3, argv), 2);
}

TEST(CloudCLIPipeline, LimitsRequiresSignIn)
{
    CloudCredentialStore::clearSession();
    CloudCredentialStore::resetCacheForTesting();

    char arg0[] = "qtmesh";
    char arg1[] = "cloud";
    char arg2[] = "limits";
    char* argv[] = {arg0, arg1, arg2};
    EXPECT_EQ(runCloud(3, argv), 1);
}

TEST(CloudCLIPipeline, StatusWhenLoggedOutPrintsDisconnected)
{
    CloudCredentialStore::clearSession();
    CloudCredentialStore::resetCacheForTesting();

    char arg0[] = "qtmesh";
    char arg1[] = "cloud";
    char arg2[] = "status";
    char* argv[] = {arg0, arg1, arg2};
    EXPECT_EQ(runCloud(3, argv), 0);
}
