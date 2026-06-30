#version 150

uniform sampler2D hdrTex;
uniform float exposureMul;
uniform int tonemapOp;
uniform float whitePoint;

in vec2 oUv0;
out vec4 fragColour;

vec3 tonemapReinhard(vec3 c)
{
    vec3 num = c * (1.0 + c / (whitePoint * whitePoint));
    return num / (1.0 + c);
}

vec3 tonemapAces(vec3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    vec3 num = x * (a * x + vec3(b));
    vec3 den = x * (c * x + vec3(d)) + vec3(e);
    return clamp(num / den, 0.0, 1.0);
}

vec3 tonemapAgx(vec3 x)
{
    x = clamp(x, 0.0, 1.0);
    vec3 agx = vec3(
        dot(x, vec3(0.224282, 0.130789, 0.044929)),
        dot(x, vec3(0.050223, 0.873461, 0.076316)),
        dot(x, vec3(0.020833, 0.080745, 0.898422)));
    return clamp(pow(max(agx, vec3(0.0)), vec3(1.35)), 0.0, 1.0);
}

vec3 linearToSrgb(vec3 c)
{
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3(0.0031308), c));
}

void main()
{
    vec3 hdr = texture(hdrTex, oUv0).rgb * exposureMul;
    vec3 mapped;
    if (tonemapOp == 1)
        mapped = tonemapAces(hdr);
    else if (tonemapOp == 2)
        mapped = tonemapAgx(hdr);
    else
        mapped = tonemapReinhard(hdr);
    fragColour = vec4(linearToSrgb(mapped), 1.0);
}
