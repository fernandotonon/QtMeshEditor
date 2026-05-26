// Minimal URP-Lit VAT replay shader.
//
// Replaces OpenVAT's stock `openVAT_decoder.shadergraph` for the demo
// because the shipped graph's VertexDescription.Position block is not
// wired to the texture-sampling subgraph — nothing the user changes on
// the material affects vertex position. This HLSL shader takes the
// same uniforms (so OpenVATEditor's material setup keeps working) and
// applies the displacement in the vertex shader.
//
// Properties mirror OpenVAT's: _openVAT_main (position+normal texture),
// _minValues / _maxValues (bake bounds), _frames, _frame, _speed,
// _UseTime, _UsePackedNormals, _exaggeration, _resolutionY, plus the
// standard PBR slots (_Basecolor, _Normal, _Metallic, _Roughness,
// _AmbientOcclusion, _Emission).

Shader "QtMeshEditor/OpenVAT_URP"
{
    Properties
    {
        _openVAT_main      ("openVAT_main",   2D)      = "white" {}
        _openVAT_nrml      ("openVAT_nrml",   2D)      = "white" {}
        _minValues         ("minValues",      Vector)  = (0, 0, 0, 0)
        _maxValues         ("maxValues",      Vector)  = (1, 1, 1, 0)
        _frame             ("frame",          Float)   = 0
        _frames            ("frames",         Float)   = 1
        _speed             ("speed",          Float)   = 1
        _resolutionY       ("resolutionY",    Float)   = 1
        _UseTime           ("UseTime",        Float)   = 1
        _UsePackedNormals  ("UsePackedNormals", Float) = 1
        _exaggeration      ("exaggeration",   Float)   = 1

        _Basecolor         ("Basecolor",       2D) = "white" {}
        _Normal            ("Normal",          2D) = "bump"  {}
        _Metallic          ("Metallic",        2D) = "black" {}
        _Roughness         ("Roughness",       2D) = "white" {}
        _AmbientOcclusion  ("AmbientOcclusion",2D) = "white" {}
        _Emission          ("Emission",        2D) = "black" {}
    }

    SubShader
    {
        Tags { "RenderType" = "Opaque" "RenderPipeline" = "UniversalPipeline" "Queue" = "Geometry" }
        LOD 100
        Cull Off

        Pass
        {
            Name "ForwardLit"
            Tags { "LightMode" = "UniversalForward" }

            HLSLPROGRAM
            #pragma vertex   vert
            #pragma fragment frag
            #pragma target 3.5

            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"
            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Lighting.hlsl"

            TEXTURE2D(_openVAT_main);   SAMPLER(sampler_openVAT_main);
            float4 _openVAT_main_TexelSize;
            TEXTURE2D(_Basecolor);      SAMPLER(sampler_Basecolor);

            CBUFFER_START(UnityPerMaterial)
                float4 _minValues;
                float4 _maxValues;
                float  _frame;
                float  _frames;
                float  _speed;
                float  _resolutionY;
                float  _UseTime;
                float  _UsePackedNormals;
                float  _exaggeration;
                float4 _Basecolor_ST;
            CBUFFER_END

            struct Attributes
            {
                float4 positionOS   : POSITION;
                float3 normalOS     : NORMAL;
                float2 uv           : TEXCOORD0;
                float2 uv2          : TEXCOORD1;   // bake-column index (kept for compatibility, FP16-quantized)
                float4 color        : COLOR;       // packed column index (R8G8 = 16-bit unsigned)
            };

            struct Varyings
            {
                float4 positionCS   : SV_POSITION;
                float3 normalWS     : TEXCOORD0;
                float3 positionWS   : TEXCOORD1;
                float2 uv           : TEXCOORD2;
            };

            // Picks the current frame index from either Time-driven or
            // explicit-frame mode, then samples the position texture
            // for this vertex. The bake-column index lives in UV2 (the
            // `--emit-uv2` flag injects it at bake time as TEXCOORD_1);
            // using SV_VertexID instead breaks the moment any importer
            // reorders vertices, which is what Unity's stock glTF
            // importer does. OpenVATEditor goes through that importer
            // when it builds the prefab — not our QtmGltfImporter — so
            // we MUST read the column index from UV2 to survive the
            // reorder.
            float3 SampleVATPosition(float2 uv2, out float3 nrm)
            {
                float safeFrames = max(_frames, 1.0);
                float t = _UseTime > 0.5 ? _Time.y * _speed * safeFrames : _frame;
                int curr = int(floor(t)) % int(safeFrames);
                if (curr < 0) curr += int(safeFrames);

                // Texture is (width = vertexCount) x (height = 2*frames).
                // Positions live in rows [0..frames-1], normals in
                // [frames..2*frames-1] when _UsePackedNormals.
                uint texW = uint(_openVAT_main_TexelSize.z);  // texelSize.z = width
                uint texH = uint(_openVAT_main_TexelSize.w);  // texelSize.w = height

                // Column index comes in pre-unpacked from the vertex
                // Color attribute as a raw integer 0..textureWidth-1
                // (see vert() — packed at import time into Color32 to
                // sidestep URP/Metal FP16 quantization on UV channels).
                int col = int(uv2.x + 0.5);
                int rowBlock = int(uv2.y + 0.5);
                int baseRow = rowBlock * int(safeFrames);
                int rowPos = baseRow + curr;
                int rowNrm = baseRow + curr + int(safeFrames);

                float u = (col + 0.5) * _openVAT_main_TexelSize.x;
                float vPos = (rowPos + 0.5) * _openVAT_main_TexelSize.y;
                float vNrm = (rowNrm + 0.5) * _openVAT_main_TexelSize.y;

                float3 posSample = SAMPLE_TEXTURE2D_LOD(_openVAT_main, sampler_openVAT_main, float2(u, vPos), 0).rgb;
                float3 nrmSample = SAMPLE_TEXTURE2D_LOD(_openVAT_main, sampler_openVAT_main, float2(u, vNrm), 0).rgb;
                nrm = normalize(nrmSample * 2.0 - 1.0);

                // OpenVAT swaps min/max on the X axis (Blender → Unity
                // left-hand fix). We honour the same convention by
                // negating X here: `_minValues.x` is the upstream max,
                // `_maxValues.x` is the upstream min.
                float3 pos = float3(
                    lerp(_minValues.x, _maxValues.x, posSample.r),
                    lerp(_minValues.y, _maxValues.y, posSample.g),
                    lerp(_minValues.z, _maxValues.z, posSample.b)
                );
                return pos;
            }

            // Inverse of sRGB→Linear so we recover the original byte
            // value from a Color channel URP gamma-corrected on upload.
            float3 LinearToSRGB(float3 lin)
            {
                float3 lo = lin * 12.92;
                float3 hi = 1.055 * pow(abs(lin), 1.0/2.4) - 0.055;
                return lerp(lo, hi, step(0.0031308, lin));
            }

            Varyings vert(Attributes IN)
            {
                Varyings OUT;
                // URP under Linear color space applies sRGB→Linear on
                // vertex Color channels by default, which destroys our
                // byte-packed integer payload (col=100 → r=0.392 →
                // gets gamma-decoded to ~0.127 → unpacked as col=32).
                // Undo by applying the inverse transform (Linear→sRGB)
                // — see LinearToSRGB() defined above the Pass block.
                float3 raw_color = LinearToSRGB(IN.color.rgb);
                float colLow  = raw_color.r * 255.0;
                float colHigh = raw_color.g * 255.0;
                float rowBlk  = raw_color.b * 255.0;
                float colF    = colLow + colHigh * 256.0;
                // DIAGNOSTIC: pass the RAW UNINTERPRETED IN.color.r
                // and IN.color.g through so the fragment can show the
                // actual byte values the GPU received. If we see solid
                // black/white blobs, Color isn't varying per-vertex.
                // If we see a clear 256-step gradient, the encoding is
                // intact at the GPU.

                // Real VAT replay using vertex Color as the column
                // source. Sample the position texture per-vertex using
                // LOAD_TEXTURE2D (integer texel coordinates — bypasses
                // any sampler-state filtering / lod selection that
                // could collapse per-vertex variation).
                float safeFrames2 = max(_frames, 1.0);
                float t2 = _UseTime > 0.5 ? _Time.y * _speed * safeFrames2 : _frame;
                int curr2 = int(floor(t2)) % int(safeFrames2);
                if (curr2 < 0) curr2 += int(safeFrames2);

                int col2 = int(colF + 0.5);
                int rowPos2 = curr2;
                int rowNrm2 = curr2 + int(safeFrames2);

                float3 posSample = LOAD_TEXTURE2D_LOD(
                    _openVAT_main, int2(col2, rowPos2), 0).rgb;
                float3 nrmSample = LOAD_TEXTURE2D_LOD(
                    _openVAT_main, int2(col2, rowNrm2), 0).rgb;

                float3 vatPos = float3(
                    lerp(_minValues.x, _maxValues.x, posSample.r),
                    lerp(_minValues.y, _maxValues.y, posSample.g),
                    lerp(_minValues.z, _maxValues.z, posSample.b)
                );
                float3 nrm = normalize(nrmSample * 2.0 - 1.0);

                float3 bindPos = IN.positionOS.xyz;
                float3 finalPos = lerp(bindPos, vatPos, saturate(_exaggeration));

                VertexPositionInputs vpi = GetVertexPositionInputs(finalPos);
                OUT.positionCS = vpi.positionCS;
                OUT.positionWS = vpi.positionWS;
                OUT.normalWS   = normalize(mul((float3x3)UNITY_MATRIX_M, nrm));
                OUT.uv         = IN.uv * _Basecolor_ST.xy + _Basecolor_ST.zw;
                return OUT;
            }

            half4 frag(Varyings IN) : SV_Target
            {
                half3 albedo = SAMPLE_TEXTURE2D(_Basecolor, sampler_Basecolor, IN.uv).rgb;

                Light mainLight = GetMainLight();
                float3 N = normalize(IN.normalWS);
                float3 L = mainLight.direction;
                float ndotl = saturate(dot(N, L));
                float3 ambient = SampleSH(N);
                float3 lit = albedo * (ambient + mainLight.color * ndotl);
                return half4(lit, 1);
            }
            ENDHLSL
        }
    }
    Fallback "Universal Render Pipeline/Lit"
}
