// QtMeshEditor HDR slice C — split-sum IBL with separate irradiance / prefilter / BRDF LUT.
// Builds on Ogre's RTSLib_IBL helpers (PrefilteredDFG_LUT, prefilteredRadiance, …).

#include "RTSLib_IBL.glsl"

vec3 sampleIrradianceCube(samplerCube irradianceTex, vec3 n)
{
    return decodeDataForIBL(textureCube(irradianceTex, n));
}

void evaluateIBLQtme(inout PixelParams pixel,
                     in vec3 vNormal,
                     in vec3 viewPos,
                     in mat4 invViewMat,
                     in sampler2D brdfLut,
                     in samplerCube irradianceTex,
                     in samplerCube prefilterTex,
                     in float prefilterMaxLod,
                     in vec3 envTint,
                     in float envIntensity,
                     inout vec3 color)
{
    vec3 shading_normal = normalize(vNormal);
    vec3 shading_view = -normalize(viewPos);
    float shading_NoV = clampNoV(abs(dot(shading_normal, shading_view)));
    vec3 shading_reflected = reflect(-shading_view, shading_normal);

    pixel.dfg = PrefilteredDFG_LUT(brdfLut, pixel.perceptualRoughness, shading_NoV);
    vec3 E = specularDFG(pixel);
    vec3 r = getSpecularDominantDirection(shading_normal, shading_reflected, pixel.roughness);

    r = normalize(mul(invViewMat, vec4(r, 0.0)).xyz);
    r.z *= -1.0;
    shading_normal = normalize(mul(invViewMat, vec4(shading_normal, 0.0)).xyz);

    vec3 Fr = E * prefilteredRadiance(prefilterTex, r, pixel.perceptualRoughness, prefilterMaxLod);
    vec3 diffuseIrradiance = sampleIrradianceCube(irradianceTex, shading_normal);
    vec3 Fd = pixel.diffuseColor * diffuseIrradiance * (1.0 - E);

    vec3 tint = envTint * envIntensity;
    Fr *= tint;
    Fd *= tint;

#ifndef USE_LINEAR_COLOURS
    color = pow(color, vec3_splat(2.2));
#endif

    color += Fr + Fd;

#ifndef USE_LINEAR_COLOURS
    color = pow(color, vec3_splat(1.0 / 2.2));
    color = saturate(color);
#endif
}
