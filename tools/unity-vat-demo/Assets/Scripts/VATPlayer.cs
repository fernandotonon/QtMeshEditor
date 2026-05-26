// VATPlayer — drop-in MonoBehaviour that plays back an OpenVAT bake
// produced by `qtmesh vat`.
//
// Wires:
//   • Loads the bake sidecar JSON to find frame_count + bounds.
//   • Creates a material on Hidden/QTM/VAT (Assets/Shaders/openvat.shader).
//   • Drives _CurrentFrame every Update at the target fps.
//   • Synthesizes the integer UV2 (col, row_block) that the shader's
//     texelFetch path needs, since `qtmesh vat`'s default glTF doesn't
//     ship a TEXCOORD_1 for Unity meshes (Unity's glTF importer drops
//     channels above UV0 unless they're authored as actual UVs).
//
// Usage:
//   1. Attach to a GameObject that has MeshFilter + MeshRenderer.
//   2. Drop the imported source.gltf's mesh asset into `sourceMesh`.
//   3. Drop the imported _pos.png into `positionTexture`.
//   4. Paste bounds_min / bounds_max / frame_count from the sidecar JSON,
//      or use the supplied BootstrapVAT helper.
//   5. Press Play.

using System;
using UnityEngine;

namespace QtMeshEditor.VAT
{
    [DisallowMultipleComponent]
    [RequireComponent(typeof(MeshFilter), typeof(MeshRenderer))]
    public class VATPlayer : MonoBehaviour
    {
        [Header("Source mesh")]
        [Tooltip("Mesh imported from the bake's source.gltf. Must be Read/Write enabled in the import inspector.")]
        public Mesh sourceMesh;

        [Header("Bake")]
        [Tooltip("Packed positions+normals texture from the bake (e.g. mixamo.com_pos.png).")]
        public Texture2D positionTexture;
        [Tooltip("Optional diffuse texture; if null the shader's BaseColor is used.")]
        public Texture2D diffuseTexture;
        [Tooltip("Total frames in the bake — read from the sidecar JSON's `Frames` field.")]
        public int frameCount = 1;
        [Tooltip("os-remap.Min from the sidecar JSON.")]
        public Vector3 boundsMin = new Vector3(-1f, -1f, -1f);
        [Tooltip("os-remap.Max from the sidecar JSON.")]
        public Vector3 boundsMax = new Vector3(1f, 1f, 1f);

        [Header("Playback")]
        [Tooltip("Frames per second to advance _CurrentFrame at.")]
        public float fps = 30f;
        [Tooltip("If false, _CurrentFrame is left to whatever a parent script writes via SetCurrentFrame().")]
        public bool selfDriven = true;
        [Tooltip("Material BaseColor when no diffuse is bound.")]
        public Color baseColor = new Color(0.85f, 0.78f, 0.65f, 1f);

        Material _material;
        float    _currentFrame;
        static bool _loggedUV1Range;

        void Start()
        {
            if (sourceMesh == null)
            {
                Debug.LogError("VATPlayer: sourceMesh is null. Drag the imported source.gltf's Mesh asset into this slot.", this);
                enabled = false;
                return;
            }
            if (positionTexture == null)
            {
                Debug.LogError("VATPlayer: positionTexture is null. Drag the bake's _pos.png Texture asset into this slot.", this);
                enabled = false;
                return;
            }

            // Verify the source mesh ships TEXCOORD_1 (UV2 in Unity-speak).
            // The custom QtmGltfImporter pulls UV1 directly from the
            // `qtmesh vat --emit-uv2` output, where each entry is an
            // integer (col, row) coordinate pointing at the vertex's
            // dedicated bake column. Without it, the shader's texelFetch
            // path samples random columns and the dancer turns into noise.
            EnsureUV2();

            // Runtime sanity-log — verifies UV1 made it through Unity's
            // mesh asset cook intact. If u-range is anywhere near
            // [0..positionTexture.width-1] this fires once per scene
            // and goes away; if u-range looks quantized (e.g.
            // [0..~5824] with gaps), Unity's build pipeline is still
            // FP16-ing the channel and the VAT replay will be
            // mangled. We only log the first instance — repeat warnings
            // for the perf grid would flood the log.
            if (!_loggedUV1Range)
            {
                _loggedUV1Range = true;
                var uv1 = sourceMesh.uv2;
                float lo = float.MaxValue, hi = float.MinValue;
                for (int j = 0; j < uv1.Length; j++)
                {
                    if (uv1[j].x < lo) lo = uv1[j].x;
                    if (uv1[j].x > hi) hi = uv1[j].x;
                }
                Debug.Log($"VATPlayer: runtime UV1 u-range on {sourceMesh.name} = [{lo}..{hi}] " +
                          $"(positionTexture width = {positionTexture.width}, " +
                          $"expected max = {positionTexture.width - 1}).");
            }

            // Bind the mesh to the MeshFilter explicitly — the user might
            // have left the slot empty.
            var mf = GetComponent<MeshFilter>();
            if (mf.sharedMesh != sourceMesh) mf.sharedMesh = sourceMesh;

            // Find the shader either at its asset path or by name.
            var shader = Shader.Find("Hidden/QTM/VAT");
            if (shader == null)
            {
                Debug.LogError("VATPlayer: shader 'Hidden/QTM/VAT' not found. Make sure Assets/Shaders/openvat.shader is in the project.", this);
                enabled = false;
                return;
            }
            _material = new Material(shader) { name = "VAT_" + name };
            _material.SetTexture("_PosTex", positionTexture);
            if (diffuseTexture != null)
            {
                _material.SetTexture("_MainTex", diffuseTexture);
                _material.SetFloat("_UseDiffuseMap", 1f);
            }
            else
            {
                _material.SetFloat("_UseDiffuseMap", 0f);
            }
            _material.SetColor("_BaseColor", baseColor);
            _material.SetInt("_FrameCount", Mathf.Max(1, frameCount));
            _material.SetVector("_BoundsMin", boundsMin);
            _material.SetVector("_BoundsMax", boundsMax);
            // Tell the shader to use the texelFetch path — UV2 is integer
            // (col, row_block), not a [0,1] float UV.
            _material.SetFloat("_SynthesizedUV2", 1f);

            // Replicate the material across every submesh slot. The Rumba
            // OBJ has 11 submeshes; with only one material assigned the
            // renderer draws submesh 0 and silently drops the other 10
            // (head + torso render, arms/legs/eyes/cigar vanish). All
            // 11 submeshes share the same VAT texture + UV0 layout, so
            // they want the same Material — just N instances of it.
            var renderer = GetComponent<MeshRenderer>();
            int subCount = sourceMesh.subMeshCount;
            if (subCount <= 1)
            {
                renderer.sharedMaterial = _material;
            }
            else
            {
                var mats = new Material[subCount];
                for (int i = 0; i < subCount; i++) mats[i] = _material;
                renderer.sharedMaterials = mats;
            }
        }

        void Update()
        {
            if (!selfDriven || _material == null) return;
            _currentFrame += Time.deltaTime * fps;
            // Modulo handled inside the shader — but keep the host-side
            // float bounded so it doesn't drift to a value large enough
            // to lose float32 precision (an hour at 30 fps is only
            // 108 000, still plenty of headroom — wrap at 1e6 to be safe).
            if (_currentFrame > 1e6f) _currentFrame -= 1e6f;
            _material.SetFloat("_CurrentFrame", _currentFrame);
        }

        /// <summary>External entry point for shared-clock perf demos: many
        /// instances reading from one driver script avoids N material
        /// updates per frame. Disable `selfDriven` and call this from
        /// the spawner's Update().</summary>
        public void SetCurrentFrame(float f)
        {
            _currentFrame = f;
            if (_material != null) _material.SetFloat("_CurrentFrame", f);
        }

        /// <summary>Fallback when the imported mesh has no TEXCOORD_1.
        /// `qtmesh vat --emit-uv2` writes a per-vertex (col, row)
        /// integer UV2 into the glTF, and the custom QtmGltfImporter
        /// preserves it — so this should be a no-op on a fresh bake.
        /// It runs as a defensive synth for the (rare) case where the
        /// bake was produced without `--emit-uv2`, or the importer
        /// dropped UV1. Logs a warning if it has to synthesize because
        /// the result is only correct when the mesh is a single
        /// primitive whose vertex order matches the texture's column
        /// order — multi-primitive meshes need real authored UV2.</summary>
        void EnsureUV2()
        {
            var existing = sourceMesh.uv2;
            if (existing != null && existing.Length == sourceMesh.vertexCount) return;

            Debug.LogWarning(
                "VATPlayer: source mesh has no TEXCOORD_1 — synthesising integer UV2 " +
                "from vertex order. Only correct for single-primitive bakes. Re-bake with " +
                "`qtmesh vat --emit-uv2` for multi-submesh assets like Mixamo characters.",
                this);

            int width = positionTexture.width;
            var uv2 = new Vector2[sourceMesh.vertexCount];
            for (int j = 0; j < sourceMesh.vertexCount; j++)
            {
                int col       = j % width;
                int rowBlock  = j / width;
                uv2[j] = new Vector2(col, rowBlock);
            }
            sourceMesh.uv2 = uv2;
            sourceMesh.UploadMeshData(false);
        }
    }
}
