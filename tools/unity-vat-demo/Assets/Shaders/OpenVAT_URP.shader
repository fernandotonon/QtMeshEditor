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

            Varyings vert(Attributes IN)
            {
                Varyings OUT;
                // Unpack column index from vertex Color (R = low byte,
                // G = high byte).
                float colLow  = IN.color.r * 255.0;
                float colHigh = IN.color.g * 255.0;
                float rowBlk  = IN.color.b * 255.0;
                float colF    = colLow + colHigh * 256.0;

                float3 nrm;
                float2 packedUV2 = float2(colF, rowBlk);
                float3 vatPos = SampleVATPosition(packedUV2, nrm);
                float3 bindPos = IN.positionOS.xyz;
                float3 finalPos = lerp(bindPos, vatPos, saturate(_exaggeration));

                VertexPositionInputs vpi = GetVertexPositionInputs(finalPos);
                OUT.positionCS = vpi.positionCS;
                OUT.positionWS = vpi.positionWS;
                OUT.normalWS   = normalize(mul((float3x3)UNITY_MATRIX_M, nrm));
                // DIAGNOSTIC: pass unpacked colF through uv.x to the
                // fragment so we can verify the GPU's view of the
                // column index.
                OUT.uv = float2(colF, IN.uv.y);
                return OUT;
            }

            half4 frag(Varyings IN) : SV_Target
            {
                // Diagnostic: red→cyan gradient based on the COLOR-packed
                // column index. Smooth gradient = Color32 lossless;
                // chunky bands = Color also being quantized somehow.
                float u = saturate(IN.uv.x / 5828.0);
                return half4(u, 1.0 - u, 0.0, 1.0);
            }
            ENDHLSL
        }
    }
    Fallback "Universal Render Pipeline/Lit"
}
