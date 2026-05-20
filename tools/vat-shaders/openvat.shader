// QtMeshEditor OpenVAT — Unity 2022+ BiRP shader
// =================================================
// Plays back a Vertex Animation Texture baked by `qtmesh vat` (or any
// sharpen3d/openvat-compatible exporter).
//
// USAGE
// -----
// 1. Drop this file into Assets/Shaders/openvat.shader.
// 2. Drop the bake folder into Assets/VAT/Bakes/<basename>/ — Unity
//    will auto-import the PNG. Click the texture and set:
//      sRGB (Color Texture): OFF
//      Filter Mode:          Point
//      Compression:          None
//      Wrap Mode:            Clamp
// 3. Add a MeshFilter + MeshRenderer to a GameObject. Assign the
//    source mesh (Read/Write enabled).
// 4. Create a Material using Hidden/QTM/VAT (this shader). Set:
//      _PosTex       — packed PNG (positions + normals)
//      _BoundsMin    — Vector3 from os-remap.Min (parse the JSON strings)
//      _BoundsMax    — Vector3 from os-remap.Max
//      _FrameCount   — os-remap.Frames
//    Drive _CurrentFrame from a MonoBehaviour Update() at your target fps.
// 5. The mesh MUST have UV2. Bakes from `qtmesh vat` don't include it
//    by default — build it in C# from VERTEX_ID using the same math
//    as `tools/godot-vat-test/scripts/VATPlayer.gd:_ensure_uv2_on_mesh`
//    (the snippet at the bottom of this file ports it).
//
// URP / HDRP
// ----------
// Swap Tags { "LightMode"="ForwardBase" } for "UniversalForward" and
// replace UnityCG.cginc / AutoLight.cginc / Lighting.cginc with URP's
// Core.hlsl. The vertex math is identical — only the lighting passes
// differ.
//
// LICENSE
// -------
// MIT. Use freely.

Shader "Hidden/QTM/VAT" {
    Properties {
        _PosTex       ("Packed Position+Normal Texture", 2D) = "white" {}
        _CurrentFrame ("Current Frame",  Float) = 0
        _FrameCount   ("Frame Count",    Int)   = 1
        _BoundsMin    ("Bounds Min",     Vector) = (0,0,0,0)
        _BoundsMax    ("Bounds Max",     Vector) = (1,1,1,0)
        _BaseColor    ("Base Color",     Color)  = (0.85, 0.78, 0.65, 1)
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Geometry" }
        LOD 100
        Cull Off
        ZWrite On
        ZTest LEqual

        Pass {
            Tags { "LightMode"="ForwardBase" }
            CGPROGRAM
            #pragma vertex   vert
            #pragma fragment frag
            #include "UnityCG.cginc"
            #include "AutoLight.cginc"
            #include "Lighting.cginc"

            // Texture2D + sampler split so we can use `.Load()` /
            // `.SampleLevel()` directly without going through the
            // legacy sampler2D path.
            Texture2D _PosTex;
            SamplerState sampler_PosTex;

            float  _CurrentFrame;
            int    _FrameCount;
            float4 _BoundsMin;
            float4 _BoundsMax;
            float4 _BaseColor;

            struct appdata {
                float4 vertex   : POSITION;
                float3 normal   : NORMAL;
                float2 uv       : TEXCOORD0;
                float2 uv2      : TEXCOORD1;
            };

            struct v2f {
                float4 pos      : SV_POSITION;
                float3 worldNrm : TEXCOORD0;
                float3 worldPos : TEXCOORD1;
                float2 uv       : TEXCOORD2;
            };

            v2f vert(appdata IN) {
                // UV2 holds per-vertex (col, base-row) into the texture
                // — see `tools/vat-shaders/openvat.gdshader` header for
                // the layout reference.
                int safeFrames = max(_FrameCount, 1);
                int curr = ((int)floor(_CurrentFrame)) % safeFrames;
                if (curr < 0) curr += safeFrames;
                int next = (curr + 1) % safeFrames;
                float blend = frac(_CurrentFrame);

                uint w, h;
                _PosTex.GetDimensions(w, h);
                float frame_step = 1.0 / (float)h;
                float2 uv_c = IN.uv2 + float2(0.0, (float)curr * frame_step);
                float2 uv_n = IN.uv2 + float2(0.0, (float)next * frame_step);

                // Position: top half. Decode normalized [0,1] back to
                // world coords via Min/Max.
                float3 p_c = _PosTex.SampleLevel(sampler_PosTex, uv_c, 0).rgb;
                float3 p_n = _PosTex.SampleLevel(sampler_PosTex, uv_n, 0).rgb;
                float3 p = lerp(p_c, p_n, blend);
                float3 modelVertex = _BoundsMin.xyz + p * (_BoundsMax.xyz - _BoundsMin.xyz);

                v2f OUT;
                OUT.pos = UnityObjectToClipPos(float4(modelVertex, 1.0));

                // Normal: lower half, shifted down 0.5 in UV V space.
                float2 uv_cn = uv_c + float2(0.0, -0.5);
                float2 uv_nn = uv_n + float2(0.0, -0.5);
                float3 n_c = _PosTex.SampleLevel(sampler_PosTex, uv_cn, 0).rgb;
                float3 n_n = _PosTex.SampleLevel(sampler_PosTex, uv_nn, 0).rgb;
                float3 n = lerp(n_c, n_n, blend) * 2.0 - 1.0;
                // Negate to compensate for QtMeshEditor's FBX → Ogre
                // import path; remove if your bake's normals look
                // correct without it.
                OUT.worldNrm = normalize(mul((float3x3)unity_ObjectToWorld, -n));
                OUT.worldPos = mul(unity_ObjectToWorld, float4(modelVertex, 1.0)).xyz;
                OUT.uv = IN.uv;
                return OUT;
            }

            fixed4 frag(v2f IN) : SV_Target {
                float3 N = normalize(IN.worldNrm);
                float3 L = normalize(_WorldSpaceLightPos0.xyz);
                float ndotl = saturate(dot(N, L));
                float3 ambient = ShadeSH9(half4(N, 1)).rgb;
                float3 col = _BaseColor.rgb * (ambient + _LightColor0.rgb * ndotl);
                return fixed4(col, 1.0);
            }
            ENDCG
        }
    }

    Fallback "Diffuse"
}

// =============================================================
// C# helper — build UV2 on the imported mesh when missing.
// =============================================================
//
// Drop this into a MonoBehaviour or editor script that runs after
// the mesh is imported. The math matches Blender OpenVAT's
// core.py:create_uv_map exactly.
//
// using UnityEngine;
//
// public static class VATMeshHelper {
//     public static void EnsureUV2(Mesh mesh, int width, int frameCount) {
//         if (mesh.uv2 != null && mesh.uv2.Length == mesh.vertexCount) return;
//         int tex_height = frameCount * 2;  // packed
//         float hpx = 0.5f / width;
//         float hpy = 0.5f / tex_height;
//         Vector2[] uv2 = new Vector2[mesh.vertexCount];
//         for (int j = 0; j < mesh.vertexCount; j++) {
//             int col = j % width;
//             int row_block = j / width;
//             // Point at the LAST pixel row of the position block, so
//             // `+ frame * frame_step` walks UP through the strip.
//             int last = row_block * frameCount + frameCount - 1;
//             uv2[j] = new Vector2(
//                 (float)col / width + hpx,
//                 1.0f - (float)last / tex_height - hpy);
//         }
//         mesh.uv2 = uv2;
//         mesh.UploadMeshData(false);
//     }
// }
