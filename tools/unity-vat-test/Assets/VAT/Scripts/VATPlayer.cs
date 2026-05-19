/*
 * VATPlayer — Unity component that drives a MeshRenderer's material
 * from a QtMeshEditor OpenVAT (sharpen3d/openvat) bake.
 *
 * Sidecar shape (OpenVAT canonical):
 *   { "os-remap": { "Min": ["...","...","..."],
 *                   "Max": ["...","...","..."],
 *                   "Frames": <int> } }
 *
 * Texture shape:
 *   16-bit RGB PNG, width = vertexCount, height = 2 × Frames.
 *   Top half  rows [0..Frames)        — positions, normalized to [Min..Max]
 *   Bottom half rows [Frames..2×Frames) — normals,  encoded (n+1)/2
 *
 * Setup:
 *   1. Drop the bake folder under Assets/VAT/Bakes/<name>/. Unity
 *      auto-imports the PNG. Click the texture and in the Inspector:
 *         sRGB (Color Texture): OFF
 *         Filter Mode: Point
 *         Compression: None
 *         Wrap Mode: Clamp
 *      (The .meta written by `bake_and_stage.sh` already sets these,
 *      but if you bake outside the staging flow verify them.)
 *   2. Add a MeshFilter + MeshRenderer to a GameObject. Assign the
 *      original mesh (Read/Write enabled on the importer).
 *   3. Add this VATPlayer component. Drag in:
 *         posTex      — the packed PNG (positions + normals)
 *         sidecarJson — the *-remap_info.json TextAsset
 *   4. The component creates a Material with the VAT shader and
 *      drives currentFrame in Update() at `fpsOverride` (OpenVAT does
 *      not carry a framerate in the sidecar).
 *
 * Multi-submesh meshes work: a Material is created per submesh,
 * each with its own _VertexOffset uniform pointing at the right
 * column slice of the bake's monolithic texture.
 */

using System;
using System.Collections.Generic;
using UnityEngine;

#if UNITY_EDITOR
using UnityEditor;
#endif

[Serializable]
public class OpenVATBlock {
    // Unity's JsonUtility doesn't allow `os-remap` (the hyphen) as a
    // C# identifier, so we hand-parse this slot from raw JSON in
    // VATPlayer. The wrapper class here is only used internally for
    // serialization-tagged float lists.
    public List<string> Min = new List<string>();
    public List<string> Max = new List<string>();
    public int Frames;
}

[RequireComponent(typeof(MeshFilter))]
[RequireComponent(typeof(MeshRenderer))]
public class VATPlayer : MonoBehaviour {

    [Tooltip("Packed VAT texture (16-bit RGB, height = 2 × Frames) baked by qtmesh vat.")]
    public Texture2D posTex;

    [Tooltip("OpenVAT sidecar JSON TextAsset emitted by qtmesh vat (*-remap_info.json).")]
    public TextAsset sidecarJson;

    [Tooltip("Shader to use. If left null, the default Hidden/QTM/VAT shader is loaded.")]
    public Shader vatShader;

    [Tooltip("Playback frames per second. OpenVAT's sidecar doesn't carry the framerate (positions are resampled at bake time), so the player drives playback at this rate. Default 30 matches qtmesh vat's --fps default.")]
    public float fpsOverride = 30f;

    [Tooltip("Optional clamp: only play this many frames at the head of the bake. -1 = full clip.")]
    public int loopFrames = -1;

    [SerializeField] private float _currentFrame = 0f;

    private int _frameCount;
    private int _vertexCount;
    private Vector3 _boundsMin;
    private Vector3 _boundsMax;
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
        if (_materials == null || _materials.Length == 0 || _frameCount <= 0) return;
        int loop = (loopFrames > 0) ? loopFrames : _frameCount;
        _currentFrame = (_currentFrame + Time.deltaTime * fpsOverride) % loop;
        for (int i = 0; i < _materials.Length; i++) {
            _materials[i].SetFloat(_shaderPropCurrentFrame, _currentFrame);
        }
    }

    /// <summary>
    /// (Re)builds per-submesh materials with correct _VertexOffset
    /// uniforms. Call after changing posTex / sidecarJson at runtime.
    /// </summary>
    public bool RebuildMaterials() {
        if (posTex == null || sidecarJson == null) return false;
        if (_mf == null || _mf.sharedMesh == null) return false;

        if (!ParseOpenVATSidecar(sidecarJson.text)) {
            return false;
        }

        // Apply texture import settings programmatically too — belt
        // and braces in case the .meta wasn't picked up.
        ApplyDataTextureSettings(posTex);

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
        for (int i = 0; i < subCount; i++) {
            var mat = new Material(shader) { name = $"{name}_VAT_sub{i}" };
            mat.SetTexture("_PosTex", posTex);
            mat.SetInt("_FrameCount",  _frameCount);
            mat.SetInt("_VertexCount", _vertexCount);
            mat.SetInt(_shaderPropVertexOffset, runningOffset);
            mat.SetVector("_BoundsMin", new Vector4(_boundsMin.x, _boundsMin.y, _boundsMin.z, 0));
            mat.SetVector("_BoundsMax", new Vector4(_boundsMax.x, _boundsMax.y, _boundsMax.z, 0));
            mat.SetFloat(_shaderPropCurrentFrame, 0f);
            _materials[i] = mat;
            runningOffset += mesh.GetSubMesh(i).vertexCount;
        }
        renderer.sharedMaterials = _materials;

        // Expand the MeshRenderer's bounds to the bake bounds so
        // Unity's culling doesn't skip the mesh whenever VAT pushes
        // vertices outside the bind-pose AABB.
        var pad = (_boundsMax - _boundsMin) * 0.1f;
        var center = (_boundsMin + _boundsMax) * 0.5f;
        var size = (_boundsMax - _boundsMin) + pad * 2f;
        mesh.bounds = new Bounds(center, size);

        if (runningOffset != _vertexCount) {
            Debug.LogWarning($"[VATPlayer] {name}: submesh vertex sum ({runningOffset}) " +
                             $"!= texture width ({_vertexCount}) — VAT replay will be wrong");
        }

        Debug.Log($"[VATPlayer] {name}: loaded {_frameCount} frames × {_vertexCount} verts " +
                  $"(across {subCount} submeshes) @ {fpsOverride} fps");
        return true;
    }

    /// <summary>
    /// Parses the OpenVAT sidecar's `os-remap` block. JsonUtility can't
    /// handle the hyphen in the key name, so we do a small manual walk
    /// instead of dragging in Newtonsoft.Json.
    /// </summary>
    private bool ParseOpenVATSidecar(string json) {
        if (string.IsNullOrEmpty(json)) {
            Debug.LogError($"[VATPlayer] {name}: empty sidecar JSON");
            return false;
        }
        int blockStart = json.IndexOf("\"os-remap\"");
        if (blockStart < 0) {
            Debug.LogError($"[VATPlayer] {name}: sidecar lacks 'os-remap' key — not an OpenVAT bake");
            return false;
        }
        // Find the opening brace of the value object and its matching
        // close. Simple brace matching — sidecar is small, shape is
        // fixed, no need for a real parser.
        int objStart = json.IndexOf('{', blockStart);
        if (objStart < 0) return false;
        int depth = 0;
        int objEnd = -1;
        for (int i = objStart; i < json.Length; i++) {
            if (json[i] == '{') depth++;
            else if (json[i] == '}') {
                depth--;
                if (depth == 0) { objEnd = i; break; }
            }
        }
        if (objEnd < 0) {
            Debug.LogError($"[VATPlayer] {name}: malformed os-remap block");
            return false;
        }
        var block = json.Substring(objStart, objEnd - objStart + 1);
        var parsed = JsonUtility.FromJson<OpenVATBlock>(block);
        if (parsed == null) {
            Debug.LogError($"[VATPlayer] {name}: failed to parse os-remap block");
            return false;
        }
        if (parsed.Min == null || parsed.Min.Count < 3 ||
            parsed.Max == null || parsed.Max.Count < 3 ||
            parsed.Frames <= 0) {
            Debug.LogError($"[VATPlayer] {name}: os-remap block has malformed Min/Max/Frames");
            return false;
        }
        _frameCount = parsed.Frames;
        _vertexCount = posTex.width;
        _boundsMin = new Vector3(
            float.Parse(parsed.Min[0], System.Globalization.CultureInfo.InvariantCulture),
            float.Parse(parsed.Min[1], System.Globalization.CultureInfo.InvariantCulture),
            float.Parse(parsed.Min[2], System.Globalization.CultureInfo.InvariantCulture));
        _boundsMax = new Vector3(
            float.Parse(parsed.Max[0], System.Globalization.CultureInfo.InvariantCulture),
            float.Parse(parsed.Max[1], System.Globalization.CultureInfo.InvariantCulture),
            float.Parse(parsed.Max[2], System.Globalization.CultureInfo.InvariantCulture));
        // Sanity: texture height should be 2 × frames.
        if (posTex.height != _frameCount * 2) {
            Debug.LogWarning($"[VATPlayer] {name}: texture height {posTex.height} != " +
                             $"2 × Frames ({_frameCount * 2}) — is this an OpenVAT bake?");
        }
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
