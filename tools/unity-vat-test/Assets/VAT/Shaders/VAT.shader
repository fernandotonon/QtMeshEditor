// VAT.shader — Unity Built-in Render Pipeline shader for replaying
// a QtMeshEditor VAT bake. Mirrors the Godot harness's embedded
// shader: row=frame, column=vertex_id+vertex_offset, normalize
// from bounds_min/max.
//
// Works in BiRP. For URP/HDRP, swap `Tags { "LightMode"="ForwardBase" }`
// for `"LightMode"="UniversalForward"` and replace `UnityCG.cginc` /
// `AutoLight.cginc` with the URP equivalents (`Core.hlsl`).

Shader "Hidden/QTM/VAT" {
    Properties {
        _PosTex       ("Position Texture",   2D)    = "white" {}
        _NrmTex       ("Normal Texture",     2D)    = "white" {}
        _HasNrmTex    ("Has Normal Map",     Float) = 0
        _CurrentFrame ("Current Frame",      Float) = 0
        _FrameCount   ("Frame Count",        Int)   = 1
        _VertexCount  ("Vertex Count",       Int)   = 1
        // Per-submesh column offset into the bake's monolithic
        // position texture. Unity's `unity_VertexID` (SV_VertexID)
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
            sampler2D _NrmTex;
            float     _HasNrmTex;
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
                float v = (_CurrentFrame + 0.5) / (float)_FrameCount;
                float3 t = tex2Dlod(_PosTex, float4(u, v, 0, 0)).rgb;
                float3 worldVertex = _BoundsMin.xyz + t * (_BoundsMax.xyz - _BoundsMin.xyz);

                v2f OUT;
                OUT.pos = UnityObjectToClipPos(float4(worldVertex, 1.0));

                if (_HasNrmTex > 0.5) {
                    float3 n = tex2Dlod(_NrmTex, float4(u, v, 0, 0)).rgb * 2.0 - 1.0;
                    OUT.worldNrm = normalize(mul((float3x3)unity_ObjectToWorld, n));
                } else {
                    // Cheap derived normal: cross of neighbour-tangent
                    // and world up. Matches the Godot fallback so the
                    // mesh has some shading variation even without
                    // a baked normal texture.
                    int nVid = clamp(globalVid + 1, 0, _VertexCount - 1);
                    float nu = (nVid + 0.5) / (float)_VertexCount;
                    float3 nT = tex2Dlod(_PosTex, float4(nu, v, 0, 0)).rgb;
                    float3 neighbourPos = _BoundsMin.xyz + nT * (_BoundsMax.xyz - _BoundsMin.xyz);
                    float3 tangent = neighbourPos - worldVertex;
                    float3 derived = normalize(cross(tangent, float3(0, 1, 0)));
                    OUT.worldNrm = mul((float3x3)unity_ObjectToWorld, derived);
                }
                OUT.worldPos = mul(unity_ObjectToWorld, float4(worldVertex, 1.0)).xyz;
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
