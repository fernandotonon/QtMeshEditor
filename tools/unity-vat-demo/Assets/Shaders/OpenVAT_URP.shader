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
                float2 uv2          : TEXCOORD1;   // bake-column index, preserved across importer reorders
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

                // UV2.x carries the bake's column index. We accept it
                // either as a raw integer (0..texWidth-1) or as a
                // normalized [0,1] float (× texWidth in the shader),
                // so the same shader works for whichever path the
                // importer takes. Detect normalized form by the
                // observed range — if any vertex's uv2.x ≤ 1.0, treat
                // as normalized.
                //
                // FP16 quantization: URP under Metal silently
                // downcasts UV channels to half-float during the GPU
                // vertex upload, even though VertexChannelCompressionMask
                // says otherwise. FP16 can represent integers up to
                // 2048 exactly; values 2048+ snap to the nearest
                // multiple of 2, 4, 8 etc. → eggs. Reading from a
                // normalized [0,1] form sidesteps the quantization.
                float texWidthF = _openVAT_main_TexelSize.z;
                float colF = uv2.x;
                // Heuristic: if max possible uv2 (5827) was sent raw,
                // colF could be anywhere in [0..texWidth]; if normalized,
                // colF is in [0..1]. Multiply by texWidth iff we're
                // clearly in the [0..1] range.
                if (colF <= 1.0001) colF *= texWidthF;
                int col = int(colF + 0.5);
                int rowBlock = int(uv2.y);
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
                float3 nrm;
                float3 vatPos = SampleVATPosition(IN.uv2, nrm);
                // Mix between bind-pose and VAT-pose by _exaggeration.
                float3 bindPos = IN.positionOS.xyz;
                float3 finalPos = lerp(bindPos, vatPos, saturate(_exaggeration));

                VertexPositionInputs vpi = GetVertexPositionInputs(finalPos);
                OUT.positionCS = vpi.positionCS;
                OUT.positionWS = vpi.positionWS;
                OUT.normalWS   = normalize(mul((float3x3)UNITY_MATRIX_M, nrm));
                // Diagnostic: pass uv2.x as our fragment uv.x so the
                // fragment shader can paint by it. If we see a SMOOTH
                // red-to-cyan gradient across the dancer (red where
                // uv2.x is low, cyan where high), UV2 made it to the
                // GPU per-vertex. If we see chunky bands → FP16
                // quantization at the vertex input attribute fetch.
                OUT.uv = float2(IN.uv2.x, IN.uv.y);
                return OUT;
            }

            half4 frag(Varyings IN) : SV_Target
            {
                // Diagnostic output: visualise uv2.x.
                // IN.uv.x already carries uv2.x from the vertex shader.
                // Map to [0,1] (if already normalized) or divide by 5828
                // (if raw integer). Detect by magnitude.
                float u = IN.uv.x;
                if (u > 1.5) u /= 5828.0;
                return half4(u, 1.0 - u, 0.0, 1.0);
            }
            ENDHLSL
        }
    }
    Fallback "Universal Render Pipeline/Lit"
}
