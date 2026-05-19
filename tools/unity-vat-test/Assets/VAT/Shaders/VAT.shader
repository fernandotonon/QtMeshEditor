// VAT.shader — Unity Built-in Render Pipeline shader for replaying
// a QtMeshEditor OpenVAT (sharpen3d/openvat) bake.
//
// Texture layout (matches the openvat reference shader's "Packed
// Normals" mode):
//   height = 2 × _FrameCount
//   rows [0 .. _FrameCount)          — positions, normalized to [Min..Max]
//   rows [_FrameCount .. 2×_FrameCount) — normals,  encoded (n+1)/2
//
// We compute V for the position half as
//   v_pos = (_CurrentFrame + 0.5) / (2 × _FrameCount)
// and for the normal half by shifting half a texture down:
//   v_nrm = v_pos + 0.5
//
// Works in BiRP. For URP/HDRP, swap `Tags { "LightMode"="ForwardBase" }`
// for `"LightMode"="UniversalForward"` and replace `UnityCG.cginc` /
// `AutoLight.cginc` with the URP equivalents (`Core.hlsl`).

Shader "Hidden/QTM/VAT" {
    Properties {
        _PosTex       ("Packed Position+Normal Texture", 2D)    = "white" {}
        _CurrentFrame ("Current Frame",      Float) = 0
        _FrameCount   ("Frame Count",        Int)   = 1
        _VertexCount  ("Vertex Count",       Int)   = 1
        // Per-submesh column offset into the bake's monolithic
        // packed texture. Unity's `unity_VertexID` (SV_VertexID)
        // is per-submesh just like Godot's VERTEX_ID, so we have to
        // add this offset to find the right column.
        _VertexOffset ("Vertex Offset",      Int)   = 0
        _BoundsMin    ("Bounds Min",         Vector) = (0,0,0,0)
        _BoundsMax    ("Bounds Max",         Vector) = (1,1,1,0)
        _BaseColor    ("Base Color",         Color) = (0.85, 0.78, 0.65, 1)
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

            sampler2D _PosTex;
            float     _CurrentFrame;
            int       _FrameCount;
            int       _VertexCount;
            int       _VertexOffset;
            float4    _BoundsMin;
            float4    _BoundsMax;
            float4    _BaseColor;

            struct appdata {
                float4 vertex   : POSITION;
                float3 normal   : NORMAL;
                uint   vid      : SV_VertexID;
            };

            struct v2f {
                float4 pos      : SV_POSITION;
                float3 worldNrm : TEXCOORD0;
                float3 worldPos : TEXCOORD1;
            };

            v2f vert(appdata IN) {
                int globalVid = _VertexOffset + (int)IN.vid;
                float u = (globalVid + 0.5) / (float)_VertexCount;

                // Position rows live in the upper half (frame_count rows
                // out of 2 × frame_count total), so divide by twice the
                // frame count. Normal rows are shifted down by half the
                // texture height (= 0.5 in UV).
                float v_pos = (_CurrentFrame + 0.5) / (float)(_FrameCount * 2);
                float v_nrm = v_pos + 0.5;

                float3 t = tex2Dlod(_PosTex, float4(u, v_pos, 0, 0)).rgb;
                float3 modelVertex = _BoundsMin.xyz + t * (_BoundsMax.xyz - _BoundsMin.xyz);

                v2f OUT;
                OUT.pos = UnityObjectToClipPos(float4(modelVertex, 1.0));

                float3 n = tex2Dlod(_PosTex, float4(u, v_nrm, 0, 0)).rgb * 2.0 - 1.0;
                OUT.worldNrm = normalize(mul((float3x3)unity_ObjectToWorld, n));
                OUT.worldPos = mul(unity_ObjectToWorld, float4(modelVertex, 1.0)).xyz;
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
