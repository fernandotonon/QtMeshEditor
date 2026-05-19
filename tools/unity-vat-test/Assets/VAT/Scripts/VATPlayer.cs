/*
 * VATPlayer — Unity component that drives a MeshRenderer's
 * material from a QtMeshEditor VAT bake (PNG + JSON sidecar).
 *
 * Setup:
 *   1. Drop the bake folder under Assets/VAT/Bakes/<name>/. Unity
 *      auto-imports the PNGs. Click the position PNG and in the
 *      Inspector:
 *         sRGB (Color Texture): OFF
 *         Filter Mode: Point
 *         Compression: None
 *         Wrap Mode: Clamp
 *      (The .meta written by `qtmesh vat --target unity` already
 *      sets these, but if you bake outside Unity verify them.)
 *   2. Add a MeshFilter + MeshRenderer to a GameObject. Assign the
 *      original mesh (Read/Write enabled on the importer).
 *   3. Add this VATPlayer component. Drag in:
 *         posTex      — the position PNG
 *         nrmTex      — the normal PNG (optional)
 *         sidecarJson — the .json TextAsset
 *      Or call LoadFromResources("VAT/Bakes/MyClip") at runtime.
 *   4. The component creates a Material with the VAT shader and
 *      drives currentFrame in Update() at the sidecar's fps.
 *
 * Multi-submesh meshes work: a Material is created per submesh,
 * each with its own vertex_offset uniform pointing at the right
 * column slice of the bake's monolithic position texture.
 */

using System;
using System.Collections.Generic;
using UnityEngine;

#if UNITY_EDITOR
using UnityEditor;
#endif

[Serializable]
public class VATSidecarBounds {
    public float[] min;
    public float[] max;
}

[Serializable]
public class VATSidecar {
    public int    version;
    public string target;
    public string encoding;
    public int    frameCount;
    public int    vertexCount;
    public float  fps;
    public string animation;
    public VATSidecarBounds bounds;
    public string posTexture;
    public string nrmTexture;
}

[RequireComponent(typeof(MeshFilter))]
[RequireComponent(typeof(MeshRenderer))]
public class VATPlayer : MonoBehaviour {

    [Tooltip("Position texture (RGBA8 or RGBA16) baked by qtmesh vat.")]
    public Texture2D posTex;

    [Tooltip("Optional normal texture (same layout). Bound when present.")]
    public Texture2D nrmTex;

    [Tooltip("Sidecar JSON TextAsset emitted by qtmesh vat.")]
    public TextAsset sidecarJson;

    [Tooltip("Shader to use. If left null, the default Hidden/QTM/VAT shader is loaded.")]
    public Shader vatShader;

    [Tooltip("True = bilinear interp on V axis (frame blending). False = nearest neighbour, bit-exact playback.")]
    public bool frameInterp = false;

    [Tooltip("Optional clamp: only play this many frames at the head of the bake. -1 = full clip.")]
    public int loopFrames = -1;

    [SerializeField] private float _currentFrame = 0f;

    private VATSidecar _sidecar;
    private Material[] _materials;
    private MeshFilter _mf;
    private int _shaderPropCurrentFrame;
    private int _shaderPropVertexOffset;

    void Awake() {
        _mf = GetComponent<MeshFilter>();
        _shaderPropCurrentFrame = Shader.PropertyToID("_CurrentFrame");
        _shaderPropVertexOffset = Shader.PropertyToID("_VertexOffset");
    }

    void OnEnable() {
        if (!RebuildMaterials()) {
            Debug.LogError($"[VATPlayer] {name}: setup failed (missing tex / sidecar / mesh)");
        }
    }

    void Update() {
        if (_sidecar == null || _materials == null || _materials.Length == 0) return;
        int loop = (loopFrames > 0) ? loopFrames : _sidecar.frameCount;
        _currentFrame = (_currentFrame + Time.deltaTime * _sidecar.fps) % loop;
        for (int i = 0; i < _materials.Length; i++) {
            _materials[i].SetFloat(_shaderPropCurrentFrame, _currentFrame);
        }
    }

    /// <summary>
    /// (Re)builds per-submesh materials with correct vertex_offset
    /// uniforms. Call after changing posTex / sidecarJson at runtime.
    /// </summary>
    public bool RebuildMaterials() {
        if (posTex == null || sidecarJson == null) return false;
        if (_mf == null || _mf.sharedMesh == null) return false;

        _sidecar = JsonUtility.FromJson<VATSidecar>(sidecarJson.text);
        if (_sidecar == null || _sidecar.frameCount <= 0 || _sidecar.vertexCount <= 0) {
            Debug.LogError($"[VATPlayer] {name}: malformed sidecar JSON");
            return false;
        }

        // Apply texture import settings programmatically too — belt
        // and braces in case the .meta wasn't picked up.
        ApplyDataTextureSettings(posTex);
        if (nrmTex != null) ApplyDataTextureSettings(nrmTex);

        var shader = vatShader != null ? vatShader : Shader.Find("Hidden/QTM/VAT");
        if (shader == null) {
            Debug.LogError("[VATPlayer] shader not found — expected Hidden/QTM/VAT or set `vatShader` manually");
            return false;
        }

        var mesh = _mf.sharedMesh;
        int subCount = mesh.subMeshCount;
        _materials = new Material[subCount];
        var renderer = GetComponent<MeshRenderer>();

        int runningOffset = 0;
        Vector3 boundsMin = new Vector3(_sidecar.bounds.min[0], _sidecar.bounds.min[1], _sidecar.bounds.min[2]);
        Vector3 boundsMax = new Vector3(_sidecar.bounds.max[0], _sidecar.bounds.max[1], _sidecar.bounds.max[2]);

        for (int i = 0; i < subCount; i++) {
            var mat = new Material(shader) { name = $"{name}_VAT_sub{i}" };
            mat.SetTexture("_PosTex", posTex);
            if (nrmTex != null) {
                mat.SetTexture("_NrmTex", nrmTex);
                mat.SetFloat("_HasNrmTex", 1f);
            } else {
                mat.SetFloat("_HasNrmTex", 0f);
            }
            mat.SetInt("_FrameCount",  _sidecar.frameCount);
            mat.SetInt("_VertexCount", _sidecar.vertexCount);
            mat.SetInt(_shaderPropVertexOffset, runningOffset);
            mat.SetVector("_BoundsMin", new Vector4(boundsMin.x, boundsMin.y, boundsMin.z, 0));
            mat.SetVector("_BoundsMax", new Vector4(boundsMax.x, boundsMax.y, boundsMax.z, 0));
            mat.SetFloat(_shaderPropCurrentFrame, 0f);
            _materials[i] = mat;

            // Advance offset by this submesh's vertex count for the next.
            runningOffset += mesh.GetSubMesh(i).vertexCount;
        }
        renderer.sharedMaterials = _materials;

        // Expand the MeshRenderer's bounds to the bake bounds so
        // Unity's culling doesn't skip the mesh whenever VAT pushes
        // vertices outside the bind-pose AABB (same issue we hit in
        // Godot).
        var pad = (boundsMax - boundsMin) * 0.1f;
        var center = (boundsMin + boundsMax) * 0.5f;
        var size = (boundsMax - boundsMin) + pad * 2f;
        mesh.bounds = new Bounds(center, size);

        if (runningOffset != _sidecar.vertexCount) {
            Debug.LogWarning($"[VATPlayer] {name}: submesh vertex sum ({runningOffset}) " +
                             $"!= bake vertexCount ({_sidecar.vertexCount}) — VAT replay will be wrong");
        }

        Debug.Log($"[VATPlayer] {name}: loaded {_sidecar.frameCount} frames × {_sidecar.vertexCount} verts " +
                  $"(across {subCount} submeshes) @ {_sidecar.fps} fps");
        return true;
    }

    private static void ApplyDataTextureSettings(Texture2D tex) {
#if UNITY_EDITOR
        var path = AssetDatabase.GetAssetPath(tex);
        if (string.IsNullOrEmpty(path)) return;
        var importer = AssetImporter.GetAtPath(path) as TextureImporter;
        if (importer == null) return;
        bool dirty = false;
        if (importer.sRGBTexture) { importer.sRGBTexture = false; dirty = true; }
        if (importer.filterMode != FilterMode.Point) { importer.filterMode = FilterMode.Point; dirty = true; }
        if (importer.wrapMode != TextureWrapMode.Clamp) { importer.wrapMode = TextureWrapMode.Clamp; dirty = true; }
        if (importer.textureCompression != TextureImporterCompression.Uncompressed) {
            importer.textureCompression = TextureImporterCompression.Uncompressed;
            dirty = true;
        }
        if (dirty) {
            importer.SaveAndReimport();
        }
#endif
    }
}
