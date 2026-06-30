#include <gtest/gtest.h>

#include "HDR/HDREnvironmentManager.h"
#include "HDR/HdrIblPrecompute.h"
#include "HDR/HdrIblRtss.h"
#include "MaterialPresetLibrary.h"
#include "RTShaderHelper.h"
#include "TestHelpers.h"

#include <OgreRTShaderSystem.h>

namespace {

HdrIbl::IblBakeResult makeTinyIblBake()
{
    HdrIbl::IblBakeResult result;
    result.irradiance.faceSize = 4;
    const size_t irrPixels = 4u * 4u * 3u;
    for (auto& face : result.irradiance.faces)
        face.assign(irrPixels, 0.5f);

    result.prefilter.mips.resize(2);
    for (int mip = 0; mip < 2; ++mip) {
        const int faceSize = 4 >> mip;
        auto& level = result.prefilter.mips[static_cast<size_t>(mip)];
        level.faceSize = faceSize;
        level.faces.faceSize = faceSize;
        const size_t pixels = static_cast<size_t>(faceSize) * static_cast<size_t>(faceSize) * 3u;
        for (auto& face : level.faces.faces)
            face.assign(pixels, 0.75f);
    }

    result.brdfLut.size = 4;
    result.brdfLut.rg.assign(4u * 4u * 2u, 0.25f);
    return result;
}

Ogre::RTShader::SubRenderState* findQtmeHdrIblSrs(Ogre::MaterialPtr& mat)
{
    auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    if (!shaderGen)
        return nullptr;
    auto* renderState = shaderGen->getRenderState(
        Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME, *mat, 0);
    if (!renderState)
        return nullptr;
    return renderState->getSubRenderState(HdrIblRtss::SRS_QTME_HDR_IBL);
}

} // namespace

class RTShaderHdrIblTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        Manager::kill();
        HDREnvironmentManager::kill();
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles());
        createStandardOgreMaterials();

        auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
        ASSERT_NE(sceneMgr, nullptr);
        RTShaderHelper::initialize(sceneMgr);
        m_rtssShutdown = [sceneMgr] { RTShaderHelper::shutdown(sceneMgr); };
    }

    void TearDown() override
    {
        if (m_rtssShutdown)
            m_rtssShutdown();
        HDREnvironmentManager::kill();
        Manager::kill();
    }

    std::function<void()> m_rtssShutdown;
};

TEST_F(RTShaderHdrIblTest, PbrMaterialGetsQtmeHdrIblSrs)
{
    auto* hdrMgr = HDREnvironmentManager::getSingleton();
    hdrMgr->setBackgroundIblPrecomputeEnabled(false);

    HdrIbl::IblBakeResult bake = makeTinyIblBake();
    const QString cacheKey = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
    QString installError;
    ASSERT_TRUE(hdrMgr->installIblBake(cacheKey, bake, installError)) << installError.toStdString();
    ASSERT_TRUE(hdrMgr->isIblReady());

    MaterialPresetLibrary::instance()->applyPreset("Metallic-Roughness");
    auto mat = Ogre::MaterialManager::getSingleton().getByName("Preset/Metallic-Roughness");
    ASSERT_TRUE(bool(mat));

    auto* pass = mat->getTechnique(0)->getPass(0);
    RTShaderHelper::setPbrEnvIntensity(pass, 2.0f);
    RTShaderHelper::setPbrEnvTint(pass, Ogre::ColourValue(0.8f, 0.9f, 1.0f));

    ASSERT_TRUE(RTShaderHelper::applyPbrIfTagged(mat));

    auto* cook = Ogre::RTShader::ShaderGenerator::getSingletonPtr()
                     ->getRenderState(Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME, *mat, 0)
                     ->getSubRenderState(Ogre::RTShader::SRS_COOK_TORRANCE_LIGHTING);
    EXPECT_NE(cook, nullptr);

    auto* ibl = findQtmeHdrIblSrs(mat);
    ASSERT_NE(ibl, nullptr) << "QtmeHdrIbl SRS missing on PBR material when IBL is ready";
    EXPECT_EQ(ibl->getType(), HdrIblRtss::SRS_QTME_HDR_IBL);
}

TEST_F(RTShaderHdrIblTest, PhongMaterialDoesNotGetQtmeHdrIblSrs)
{
    auto* hdrMgr = HDREnvironmentManager::getSingleton();
    hdrMgr->setBackgroundIblPrecomputeEnabled(false);

    HdrIbl::IblBakeResult bake = makeTinyIblBake();
    const QString cacheKey = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
    QString installError;
    ASSERT_TRUE(hdrMgr->installIblBake(cacheKey, bake, installError));

    MaterialPresetLibrary::instance()->applyPreset("Plastic (Red)");
    auto mat = Ogre::MaterialManager::getSingleton().getByName("Preset/Plastic (Red)");
    ASSERT_TRUE(bool(mat));

    EXPECT_FALSE(RTShaderHelper::applyPbrIfTagged(mat));
    EXPECT_EQ(findQtmeHdrIblSrs(mat), nullptr);
}

TEST_F(RTShaderHdrIblTest, PbrMaterialWithoutIblDoesNotAttachQtmeHdrIblSrs)
{
    MaterialPresetLibrary::instance()->applyPreset("Metallic-Roughness");
    auto mat = Ogre::MaterialManager::getSingleton().getByName("Preset/Metallic-Roughness");
    ASSERT_TRUE(bool(mat));

    ASSERT_TRUE(RTShaderHelper::applyPbrIfTagged(mat));
    EXPECT_EQ(findQtmeHdrIblSrs(mat), nullptr)
        << "IBL SRS must not attach when no environment is loaded";
}
