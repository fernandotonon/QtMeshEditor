/*
 * VATTestSetup — Editor menu command that builds the side-by-side
 * comparison scene programmatically. Avoids checking a giant
 * `.unity` YAML file into the repo (and the version-locking
 * headaches that come with it).
 *
 * Usage:
 *   QtMeshEditor → VAT → Build Test Scene
 *
 * Scans Assets/VAT/Bakes/*\/ for a sidecar+textures+source.gltf
 * triple, instantiates the source mesh's prefab on the left
 * (skeletal, ground truth) and on the right (VAT-driven), and
 * sets up the camera + light.
 */

using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

public static class VATTestSetup {

    [MenuItem("QtMeshEditor/VAT/Build Test Scene")]
    public static void BuildTestScene() {
        var bakeDir = FindFirstBakeDir();
        if (string.IsNullOrEmpty(bakeDir)) {
            EditorUtility.DisplayDialog(
                "VAT Test Setup",
                "No bake found under Assets/VAT/Bakes/.\n\n" +
                "Run tools/unity-vat-test/bake_and_stage.sh first.",
                "OK");
            return;
        }
        Debug.Log($"[VATTestSetup] using bake: {bakeDir}");

        // Create a fresh scene
        var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
        scene.name = "VATTest";

        // Camera
        var camGO = new GameObject("Main Camera");
        var cam = camGO.AddComponent<Camera>();
        cam.tag = "MainCamera";
        cam.transform.position = new Vector3(0, 1.4f, 3.5f);
        cam.transform.rotation = Quaternion.Euler(-22.5f, 0, 0);
        cam.fieldOfView = 50f;
        camGO.AddComponent<AudioListener>();
        cam.backgroundColor = new Color(0.1f, 0.11f, 0.13f);
        cam.clearFlags = CameraClearFlags.SolidColor;

        // Light
        var lightGO = new GameObject("Directional Light");
        var light = lightGO.AddComponent<Light>();
        light.type = LightType.Directional;
        light.intensity = 1.2f;
        lightGO.transform.rotation = Quaternion.Euler(50, -30, 0);

        // Floor
        var floor = GameObject.CreatePrimitive(PrimitiveType.Plane);
        floor.name = "Floor";
        floor.transform.localScale = new Vector3(0.8f, 1, 0.8f);
        var floorMat = new Material(Shader.Find("Standard"));
        floorMat.color = new Color(0.18f, 0.18f, 0.20f);
        floor.GetComponent<Renderer>().sharedMaterial = floorMat;

        // Find the bake's source mesh, position texture, normal texture, sidecar
        string posTexPath = Directory.GetFiles(bakeDir, "*_pos.png").FirstOrDefault();
        string nrmTexPath = Directory.GetFiles(bakeDir, "*_nrm.png").FirstOrDefault();
        string sidecarPath = Directory.GetFiles(bakeDir, "*.json").FirstOrDefault();
        string gltfPath = Path.Combine(bakeDir, "source.gltf");
        if (posTexPath == null || sidecarPath == null || !File.Exists(gltfPath)) {
            EditorUtility.DisplayDialog("VAT Test Setup",
                "Bake folder is incomplete (missing PNG / JSON / source.gltf).",
                "OK");
            return;
        }
        AssetDatabase.Refresh();
        var posTex     = AssetDatabase.LoadAssetAtPath<Texture2D>(posTexPath);
        var nrmTex     = nrmTexPath != null ? AssetDatabase.LoadAssetAtPath<Texture2D>(nrmTexPath) : null;
        var sidecarTxt = AssetDatabase.LoadAssetAtPath<TextAsset>(sidecarPath);
        var gltfPrefab = AssetDatabase.LoadAssetAtPath<GameObject>(gltfPath);

        if (gltfPrefab == null) {
            EditorUtility.DisplayDialog("VAT Test Setup",
                "Unity didn't import source.gltf as a prefab yet.\n\n" +
                "Right-click on it in the Project pane → Reimport, then re-run this menu.",
                "OK");
            return;
        }

        // LEFT: live-skinned ground truth (just instantiate the glTF —
        // Unity auto-creates the SkinnedMeshRenderer + Animation).
        var leftGO = (GameObject)PrefabUtility.InstantiatePrefab(gltfPrefab);
        leftGO.name = "Skeletal (live)";
        leftGO.transform.position = new Vector3(-0.9f, 0, 0);
        AddLabel(leftGO, "Skeletal (live)", new Color(0.45f, 0.95f, 0.45f));

        // RIGHT: VAT replay. Instantiate the same prefab, then convert
        // its SkinnedMeshRenderer into a plain MeshRenderer + MeshFilter
        // (we don't want Unity's skinning competing with the VAT shader).
        var rightGO = (GameObject)PrefabUtility.InstantiatePrefab(gltfPrefab);
        rightGO.name = "VAT (replay)";
        rightGO.transform.position = new Vector3(0.9f, 0, 0);
        ConvertToVATTarget(rightGO, posTex, nrmTex, sidecarTxt);
        AddLabel(rightGO, "VAT (replay)", new Color(0.55f, 0.7f, 1f));

        EditorSceneManager.SaveScene(scene, "Assets/VAT/Scenes/VATTest.unity");
        Debug.Log("[VATTestSetup] scene built and saved to Assets/VAT/Scenes/VATTest.unity");
    }

    private static string FindFirstBakeDir() {
        const string root = "Assets/VAT/Bakes";
        if (!Directory.Exists(root)) return null;
        foreach (var d in Directory.GetDirectories(root)) {
            if (Directory.GetFiles(d, "*.json").Length > 0
                && Directory.GetFiles(d, "*_pos.png").Length > 0
                && File.Exists(Path.Combine(d, "source.gltf"))) {
                return d;
            }
        }
        return null;
    }

    private static void ConvertToVATTarget(GameObject root, Texture2D posTex,
                                           Texture2D nrmTex, TextAsset sidecarTxt) {
        var skinned = root.GetComponentInChildren<SkinnedMeshRenderer>();
        if (skinned == null) {
            Debug.LogError("[VATTestSetup] expected SkinnedMeshRenderer in the glTF prefab");
            return;
        }
        // Snapshot the shared mesh + transform from the skinned renderer,
        // then strip the skinning machinery.
        var mesh = skinned.sharedMesh;
        var targetGO = skinned.gameObject;
        Object.DestroyImmediate(skinned);
        // Also drop Animator/Animation so they don't compete.
        var animator = targetGO.GetComponentInParent<Animator>();
        if (animator != null) Object.DestroyImmediate(animator);
        // Strip any leftover Animation component too.
        foreach (var a in root.GetComponentsInChildren<Animation>()) {
            Object.DestroyImmediate(a);
        }

        var mf = targetGO.AddComponent<MeshFilter>();
        mf.sharedMesh = mesh;
        var mr = targetGO.AddComponent<MeshRenderer>();
        // VATPlayer creates per-submesh materials in RebuildMaterials.

        var player = targetGO.AddComponent<VATPlayer>();
        player.posTex      = posTex;
        player.nrmTex      = nrmTex;
        player.sidecarJson = sidecarTxt;
        player.vatShader   = Shader.Find("Hidden/QTM/VAT");
    }

    private static void AddLabel(GameObject root, string text, Color color) {
        var labelGO = new GameObject("Label");
        labelGO.transform.SetParent(root.transform, false);
        labelGO.transform.localPosition = new Vector3(0, 2.1f, 0);
        var tm = labelGO.AddComponent<TextMesh>();
        tm.text = text;
        tm.fontSize = 32;
        tm.color = color;
        tm.anchor = TextAnchor.MiddleCenter;
        tm.alignment = TextAlignment.Center;
        tm.characterSize = 0.05f;
    }
}
