// LCOV_EXCL_START — requires initialized Ogre RTSS with GPU/render system

#include "HDR/HdrIblRtss.h"

#include <OgreLogManager.h>
#include <OgreShaderFFPRenderState.h>
#include <OgreShaderParameter.h>
#include <OgreShaderProgram.h>
#include <OgreShaderSubRenderState.h>

namespace HdrIblRtss {
namespace {

const Ogre::String kType = "QtmeHdrIbl";

const char* kParamIrradiance = "irradiance_texture";
const char* kParamPrefilter = "prefilter_texture";
const char* kParamBrdfLut = "brdf_lut_texture";
const char* kParamMaxLod = "prefilter_max_lod";
const char* kParamIntensity = "env_intensity";
const char* kParamTint = "env_tint";

class QtmeHdrIblLighting : public Ogre::RTShader::SubRenderState
{
public:
    const Ogre::String& getType() const override { return kType; }
    int getExecutionOrder() const override { return Ogre::RTShader::FFP_LIGHTING + 10; }

    bool setParameter(const Ogre::String& name, const Ogre::String& value) override
    {
        if (name == kParamIrradiance && !value.empty()) {
            mIrradianceName = value;
            return true;
        }
        if (name == kParamPrefilter && !value.empty()) {
            mPrefilterName = value;
            return true;
        }
        if (name == kParamBrdfLut && !value.empty()) {
            mBrdfLutName = value;
            return true;
        }
        if (name == kParamMaxLod) {
            mPrefilterMaxLod = Ogre::StringConverter::parseReal(value);
            mGpuParamsDirty = true;
            return true;
        }
        if (name == kParamIntensity) {
            mEnvIntensity = Ogre::StringConverter::parseReal(value);
            mGpuParamsDirty = true;
            return true;
        }
        if (name == kParamTint) {
            const Ogre::StringVector parts = Ogre::StringUtil::split(value, " ");
            if (parts.size() >= 3) {
                mEnvTint.r = Ogre::StringConverter::parseReal(parts[0]);
                mEnvTint.g = Ogre::StringConverter::parseReal(parts[1]);
                mEnvTint.b = Ogre::StringConverter::parseReal(parts[2]);
                mGpuParamsDirty = true;
                return true;
            }
        }
        return false;
    }

    void copyFrom(const Ogre::RTShader::SubRenderState& rhs) override
    {
        const auto& other = static_cast<const QtmeHdrIblLighting&>(rhs);
        mIrradianceName = other.mIrradianceName;
        mPrefilterName = other.mPrefilterName;
        mBrdfLutName = other.mBrdfLutName;
        mPrefilterMaxLod = other.mPrefilterMaxLod;
        mEnvIntensity = other.mEnvIntensity;
        mEnvTint = other.mEnvTint;
        mGpuParamsDirty = true;
    }

    bool preAddToRenderState(const Ogre::RTShader::RenderState* /*renderState*/,
                             Ogre::Pass* /*srcPass*/,
                             Ogre::Pass* dstPass) override
    {
        if (mIrradianceName.empty() || mPrefilterName.empty() || mBrdfLutName.empty())
            return false;

        auto* brdfTus = dstPass->createTextureUnitState();
        brdfTus->setTextureName(mBrdfLutName);
        brdfTus->setTextureFiltering(Ogre::TFO_TRILINEAR);
        mBrdfSamplerIndex = static_cast<int>(dstPass->getNumTextureUnitStates()) - 1;

        auto* irrTus = dstPass->createTextureUnitState();
        irrTus->setTextureName(mIrradianceName, Ogre::TEX_TYPE_CUBE_MAP);
        irrTus->setTextureFiltering(Ogre::TFO_TRILINEAR);
        mIrradianceSamplerIndex = static_cast<int>(dstPass->getNumTextureUnitStates()) - 1;

        auto* preTus = dstPass->createTextureUnitState();
        preTus->setTextureName(mPrefilterName, Ogre::TEX_TYPE_CUBE_MAP);
        preTus->setTextureFiltering(Ogre::TFO_TRILINEAR);
        mPrefilterSamplerIndex = static_cast<int>(dstPass->getNumTextureUnitStates()) - 1;

        return true;
    }

    bool createCpuSubPrograms(Ogre::RTShader::ProgramSet* programSet) override
    {
        Ogre::RTShader::Program* vsProgram = programSet->getCpuProgram(Ogre::GPT_VERTEX_PROGRAM);
        Ogre::RTShader::Function* vsMain = vsProgram->getEntryPointFunction();
        Ogre::RTShader::Program* psProgram = programSet->getCpuProgram(Ogre::GPT_FRAGMENT_PROGRAM);
        Ogre::RTShader::Function* psMain = psProgram->getEntryPointFunction();

        auto vsOutViewPos = vsMain->resolveOutputParameter(Ogre::RTShader::Parameter::SPC_POSITION_VIEW_SPACE);
        auto viewPos = psMain->resolveInputParameter(vsOutViewPos);

        auto pixel = psMain->getLocalParameter("pixel");
        if (!pixel) {
            Ogre::LogManager::getSingleton().logError(
                "QtmeHdrIbl must be used with the Cook-Torrance (metal_roughness) SRS");
            return true;
        }

        auto viewNormal = psMain->getLocalParameter(Ogre::RTShader::Parameter::SPC_NORMAL_VIEW_SPACE);
        if (!viewNormal) {
            auto vsOutNormal = vsMain->resolveOutputParameter(Ogre::RTShader::Parameter::SPC_NORMAL_VIEW_SPACE);
            viewNormal = psMain->resolveInputParameter(vsOutNormal);
        }

        psProgram->addDependency("SGXLib_CookTorrance");
        psProgram->addDependency("QTSLib_IBL");

        mIntensityParam = psProgram->resolveParameter(Ogre::GCT_FLOAT1, "qtmeEnvIntensity");
        mTintParam = psProgram->resolveParameter(Ogre::GCT_FLOAT3, "qtmeEnvTint");
        mMaxLodParam = psProgram->resolveParameter(Ogre::GCT_FLOAT1, "qtmePrefilterMaxLod");

        auto outDiffuse = psMain->resolveOutputParameter(Ogre::RTShader::Parameter::SPC_COLOR_DIFFUSE);
        auto brdfSampler = psProgram->resolveParameter(Ogre::GCT_SAMPLER2D, "qtmeBrdfLut", mBrdfSamplerIndex);
        auto irradianceSampler =
            psProgram->resolveParameter(Ogre::GCT_SAMPLERCUBE, "qtmeIrradiance", mIrradianceSamplerIndex);
        auto prefilterSampler =
            psProgram->resolveParameter(Ogre::GCT_SAMPLERCUBE, "qtmePrefilter", mPrefilterSamplerIndex);
        auto invViewMat = psProgram->resolveParameter(Ogre::GpuProgramParameters::ACT_INVERSE_VIEW_MATRIX);

        auto fstage = psMain->getStage(355);
        fstage.callFunction(
            "evaluateIBLQtme",
            {Ogre::RTShader::InOut(pixel),
             Ogre::RTShader::In(viewNormal),
             Ogre::RTShader::In(viewPos),
             Ogre::RTShader::In(invViewMat),
             Ogre::RTShader::In(brdfSampler),
             Ogre::RTShader::In(irradianceSampler),
             Ogre::RTShader::In(prefilterSampler),
             Ogre::RTShader::In(mMaxLodParam),
             Ogre::RTShader::In(mTintParam),
             Ogre::RTShader::In(mIntensityParam),
             Ogre::RTShader::InOut(outDiffuse).xyz()});

        return true;
    }

    void updateGpuProgramsParams(Ogre::Renderable* /*rend*/,
                                 const Ogre::Pass* pass,
                                 const Ogre::AutoParamDataSource* /*source*/,
                                 const Ogre::LightList* /*ll*/) override
    {
        if (pass) {
            const float intensity = readEnvIntensity(pass);
            const Ogre::ColourValue tint = readEnvTint(pass);
            if (intensity != mEnvIntensity || tint != mEnvTint) {
                mEnvIntensity = intensity;
                mEnvTint = tint;
                mGpuParamsDirty = true;
            }
        }
        if (!mGpuParamsDirty)
            return;
        if (mIntensityParam)
            mIntensityParam->setGpuParameter(mEnvIntensity);
        if (mTintParam)
            mTintParam->setGpuParameter(mEnvTint);
        if (mMaxLodParam)
            mMaxLodParam->setGpuParameter(mPrefilterMaxLod);
        mGpuParamsDirty = false;
    }

private:
    Ogre::String mIrradianceName;
    Ogre::String mPrefilterName;
    Ogre::String mBrdfLutName;
    float mPrefilterMaxLod = 5.0f;
    float mEnvIntensity = 1.0f;
    Ogre::ColourValue mEnvTint = Ogre::ColourValue::White;

    int mBrdfSamplerIndex = 0;
    int mIrradianceSamplerIndex = 0;
    int mPrefilterSamplerIndex = 0;

    Ogre::RTShader::UniformParameterPtr mIntensityParam;
    Ogre::RTShader::UniformParameterPtr mTintParam;
    Ogre::RTShader::UniformParameterPtr mMaxLodParam;
    bool mGpuParamsDirty = true;
};

class QtmeHdrIblFactory : public Ogre::RTShader::SubRenderStateFactory
{
public:
    const Ogre::String& getType() const override { return kType; }

protected:
    Ogre::RTShader::SubRenderState* createInstanceImpl() override
    {
        return OGRE_NEW QtmeHdrIblLighting; // NOSONAR — Ogre factory pattern
    }
};

QtmeHdrIblFactory s_factory;
bool s_factoryRegistered = false;

} // namespace

const Ogre::String SRS_QTME_HDR_IBL = kType;
const char* kPbrEnvIntensityKey = "pbr_env_intensity";
const char* kPbrEnvTintKey = "pbr_env_tint";

float readEnvIntensity(const Ogre::Pass* pass)
{
    if (!pass)
        return 1.0f;
    const auto any = pass->getUserObjectBindings().getUserAny(kPbrEnvIntensityKey);
    if (!any.has_value())
        return 1.0f;
    try {
        return Ogre::any_cast<float>(any);
    } catch (...) {
        return 1.0f;
    }
}

Ogre::ColourValue readEnvTint(const Ogre::Pass* pass)
{
    if (!pass)
        return Ogre::ColourValue::White;
    const auto any = pass->getUserObjectBindings().getUserAny(kPbrEnvTintKey);
    if (!any.has_value())
        return Ogre::ColourValue::White;
    try {
        return Ogre::any_cast<Ogre::ColourValue>(any);
    } catch (...) {
        return Ogre::ColourValue::White;
    }
}

void registerFactory()
{
    if (s_factoryRegistered)
        return;
    auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    if (!shaderGen)
        return;
    shaderGen->addSubRenderStateFactory(&s_factory);
    s_factoryRegistered = true;
}

void unregisterFactory()
{
    if (!s_factoryRegistered)
        return;
    if (auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr())
        shaderGen->removeSubRenderStateFactory(&s_factory);
    s_factoryRegistered = false;
}

} // namespace HdrIblRtss

// LCOV_EXCL_STOP
