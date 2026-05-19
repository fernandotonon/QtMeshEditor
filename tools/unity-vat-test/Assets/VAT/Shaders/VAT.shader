// VAT.shader — Unity Built-in Render Pipeline shader for replaying
// a QtMeshEditor OpenVAT (sharpen3d/openvat) bake.
//
// Texture layout (matches the openvat reference shader's "Packed
// Normals" mode):
//   height = 2 × _FrameCount
//   rows [0 .. _FrameCount)            — positions, normalized to [Min..Max]
//   rows [_FrameCount .. 2×_FrameCount) — normals,  encoded (n+1)/2
//
// Frame sampling: we compute integer row indices (curr, next) from
// `_CurrentFrame`, fetch both rows via `Load()` (Unity's texelFetch —
// bypasses the sampler so nearest-filter rounding at the half-texture
// boundary can't slip a position sample into the normal half), then
// lerp() based on the fractional part of `_CurrentFrame`. Mirrors
// sharpen3d's reference shader behavior: integer rows + manual blend,
// no reliance on sampler rounding.
//
// Normals are negated on read — the FBX → Ogre import applies
// `aiProcess_ConvertToLeftHanded` which flips winding without flipping
// the captured normal vector, so the baked normals point inward.
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

            // Texture2D + sampler split so we can call `.Load()` for
            // texelFetch-style integer-coordinate sampling. The sampler
            // is still wired (some Unity paths still query it for
            // metadata) but we never sample through it.
            Texture2D _PosTex;
            SamplerState sampler_PosTex;

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

                // Manual row arithmetic — _CurrentFrame is continuous,
                // but the texture is row-addressed. Split into integer
                // current/next rows and a fractional blend, fetch both
                // rows directly via Load(), then lerp. Avoids the
                // half-texture boundary glitch where a sampler's
                // nearest-rounding picks a row from the normal half
                // when sampling the position half (one-frame blob).
                int safeFrameCount = max(_FrameCount, 1);
                int currRow = ((int)floor(_CurrentFrame)) % safeFrameCount;
                if (currRow < 0) currRow += safeFrameCount;
                int nextRow = (currRow + 1) % safeFrameCount;
                float blend = frac(_CurrentFrame);

                // Position: integer-coord fetches from the top half.
                float3 pCurr = _PosTex.Load(int3(globalVid, currRow, 0)).rgb;
                float3 pNext = _PosTex.Load(int3(globalVid, nextRow, 0)).rgb;
                float3 p = lerp(pCurr, pNext, blend);
                float3 modelVertex = _BoundsMin.xyz + p * (_BoundsMax.xyz - _BoundsMin.xyz);

                v2f OUT;
                OUT.pos = UnityObjectToClipPos(float4(modelVertex, 1.0));

                // Normal: rows in the bottom half [N..2N-1].
                float3 nCurr = _PosTex.Load(int3(globalVid, currRow + safeFrameCount, 0)).rgb;
                float3 nNext = _PosTex.Load(int3(globalVid, nextRow + safeFrameCount, 0)).rgb;
                float3 n = lerp(nCurr, nNext, blend) * 2.0 - 1.0;
                // See header comment for why we negate.
                OUT.worldNrm = normalize(mul((float3x3)unity_ObjectToWorld, -n));
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
