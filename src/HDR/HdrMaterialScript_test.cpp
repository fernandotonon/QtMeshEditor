#include "HDR/HdrMaterialScript.h"

#include <gtest/gtest.h>

TEST(HdrMaterialScriptTest, RoundTripEnvironmentLines)
{
    const QString base = QStringLiteral(
        "material TestMat\n"
        "{\n"
        "    technique\n"
        "    {\n"
        "        pass\n"
        "        {\n"
        "            lighting on\n"
        "        }\n"
        "    }\n"
        "}");

    const QColor tint = QColor::fromRgbF(0.9, 0.8, 0.7);
    const QString injected = HdrMaterialScript::injectEnvironmentLines(base, 2.5f, tint);
    EXPECT_TRUE(injected.contains(QStringLiteral("pbr_environment_intensity 2.500")));
    EXPECT_TRUE(injected.contains(QStringLiteral("pbr_environment_tint 0.900 0.800 0.700")));

    float intensity = 0.f;
    QColor parsed;
    ASSERT_TRUE(HdrMaterialScript::parseEnvironmentLines(injected, intensity, parsed));
    EXPECT_NEAR(intensity, 2.5f, 1e-4f);
    EXPECT_NEAR(parsed.redF(), 0.9f, 1e-4f);
    EXPECT_NEAR(parsed.greenF(), 0.8f, 1e-4f);
    EXPECT_NEAR(parsed.blueF(), 0.7f, 1e-4f);

    const QString stripped = HdrMaterialScript::stripEnvironmentLines(injected);
    EXPECT_FALSE(stripped.contains(QStringLiteral("pbr_environment_intensity")));
    EXPECT_TRUE(stripped.contains(QStringLiteral("lighting on")));
}

TEST(HdrMaterialScriptTest, ParseDefaultsWhenMissing)
{
    float intensity = -1.f;
    QColor tint;
    EXPECT_FALSE(HdrMaterialScript::parseEnvironmentLines(QStringLiteral("material X {}"),
                                                            intensity,
                                                            tint));
    EXPECT_FLOAT_EQ(intensity, HdrMaterialScript::kDefaultEnvIntensity);
    EXPECT_EQ(tint, QColor::fromRgbF(1., 1., 1.));
}
