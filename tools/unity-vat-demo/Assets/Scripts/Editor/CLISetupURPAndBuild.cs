// CLISetupURPAndBuild — one-shot CLI entry point that switches the
// project to URP, runs OpenVAT's official content processor against
// our bake, and builds the .app. Designed to be invoked from the
// repo root as:
//
//   /Applications/Unity/Hub/Editor/<ver>/Unity.app/Contents/MacOS/Unity \
//     -batchmode -nographics -quit \
//     -projectPath tools/unity-vat-demo \
//     -executeMethod QtMeshEditor.VAT.Editor.CLISetupURPAndBuild.Run
//
// The custom BiRP shader path (Assets/Shaders/openvat.shader +
// QtmGltfImporter + the egg-shape rabbit hole) is left in place but
// is no longer the path the build uses — URP + OpenVAT's Shader Graph
// decoder is the supported route.

#if UNITY_EDITOR
using System;
using System.IO;
using System.Linq;
using System.Reflection;
using UnityEditor;
using UnityEditor.Rendering;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Rendering;

namespace QtMeshEditor.VAT.Editor
{
    public static class CLISetupURPAndBuild
    {
        public static void Run()
        {
            try
            {
                Debug.Log("CLISetupURPAndBuild: starting URP + OpenVAT pipeline");
                EnsureColorSpace();
                EnsureURPAsset();
                RunOpenVATEditor();
                BuildScene();
                BuildPlayer();
                Debug.Log("CLISetupURPAndBuild: done");
                EditorApplication.Exit(0);
            }
            catch (Exception e)
            {
                Debug.LogError("CLISetupURPAndBuild failed: " + e);
                EditorApplication.Exit(1);
            }
        }

        // URP wants Linear color space (Gamma + URP produces washed-out lighting).
        static void EnsureColorSpace()
        {
            if (PlayerSettings.colorSpace != ColorSpace.Linear)
            {
                PlayerSettings.colorSpace = ColorSpace.Linear;
                AssetDatabase.SaveAssets();
                Debug.Log("CLISetupURPAndBuild: set color space → Linear");
            }
        }

        // Create a UniversalRenderPipelineAsset programmatically and
        // bind it to GraphicsSettings. The class lives in the
        // com.unity.render-pipelines.universal assembly; reach it via
        // reflection so this script compiles even when URP isn't
        // installed yet (the manifest install happens earlier in the
        // same editor session).
        static void EnsureURPAsset()
        {
            // Already wired up?
            if (GraphicsSettings.defaultRenderPipeline != null)
            {
                Debug.Log("CLISetupURPAndBuild: GraphicsSettings.defaultRenderPipeline already set, skipping URP wiring");
                return;
            }

            const string assetPath = "Assets/Settings/URP-Asset.asset";
            Directory.CreateDirectory("Assets/Settings");

            RenderPipelineAsset asset = AssetDatabase.LoadAssetAtPath<RenderPipelineAsset>(assetPath);
            if (asset == null)
            {
                // Find UniversalRenderPipelineAsset type via reflection.
                Type urpAssetType = AppDomain.CurrentDomain.GetAssemblies()
                    .SelectMany(a => SafeGetTypes(a))
                    .FirstOrDefault(t => t != null && t.FullName == "UnityEngine.Rendering.Universal.UniversalRenderPipelineAsset");

                if (urpAssetType == null)
                {
                    throw new Exception("URP type 'UniversalRenderPipelineAsset' not found. Did the package install fail?");
                }

                // Create a default URP asset + accompanying
                // UniversalRendererData. URP requires at least one
                // renderer in the asset's renderer list; without it
                // the SRP path bails at startup with "no renderers".
                ScriptableObject instance = ScriptableObject.CreateInstance(urpAssetType);
                AssetDatabase.CreateAsset(instance, assetPath);

                Type rendererDataType = AppDomain.CurrentDomain.GetAssemblies()
                    .SelectMany(a => SafeGetTypes(a))
                    .FirstOrDefault(t => t != null && t.FullName == "UnityEngine.Rendering.Universal.UniversalRendererData");
                if (rendererDataType != null)
                {
                    const string rendererPath = "Assets/Settings/URP-Renderer.asset";
                    ScriptableObject rendererData = ScriptableObject.CreateInstance(rendererDataType);
                    AssetDatabase.CreateAsset(rendererData, rendererPath);

                    // Set the renderer list field via reflection. The field
                    // is `m_RendererDataList` (UnityEngine.Rendering.Universal
                    // exposes the public API through `rendererDataList`).
                    FieldInfo listField = urpAssetType.GetField("m_RendererDataList",
                        BindingFlags.Instance | BindingFlags.NonPublic);
                    if (listField != null)
                    {
                        // Build a Renderer2D[]-ish array using reflection.
                        var arr = Array.CreateInstance(listField.FieldType.GetElementType()
                            ?? rendererDataType.BaseType ?? rendererDataType, 1);
                        arr.SetValue(rendererData, 0);
                        listField.SetValue(instance, arr);
                    }
                    FieldInfo defField = urpAssetType.GetField("m_DefaultRendererIndex",
                        BindingFlags.Instance | BindingFlags.NonPublic);
                    if (defField != null) defField.SetValue(instance, 0);
                }
                AssetDatabase.SaveAssets();
                asset = instance as RenderPipelineAsset;
                Debug.Log($"CLISetupURPAndBuild: created URP asset at {assetPath}");
            }

            GraphicsSettings.defaultRenderPipeline = asset;
            // Per-quality-level too — without this, lower quality levels
            // fall back to BiRP.
            int qlCount = QualitySettings.names.Length;
            for (int i = 0; i < qlCount; i++)
            {
                QualitySettings.SetQualityLevel(i, applyExpensiveChanges: false);
                QualitySettings.renderPipeline = asset;
            }
            AssetDatabase.SaveAssets();
            Debug.Log("CLISetupURPAndBuild: wired URP asset into GraphicsSettings + every QualityLevel");
        }

        // Invoke OpenVATEditor.ProcessOpenVATContent() via reflection
        // (the method is private on the EditorWindow).
        static void RunOpenVATEditor()
        {
            const string folder = "Assets/OpenVATContent";
            if (!Directory.Exists(folder))
            {
                throw new Exception($"folder {folder} doesn't exist — copy the bake into it first");
            }

            Type openVatEditorType = AppDomain.CurrentDomain.GetAssemblies()
                .SelectMany(a => SafeGetTypes(a))
                .FirstOrDefault(t => t != null && t.Name == "OpenVATEditor");

            if (openVatEditorType == null)
            {
                throw new Exception("OpenVATEditor type not found. Did the com.lukestilson.openvat package install fail?");
            }

            ScriptableObject instance = ScriptableObject.CreateInstance(openVatEditorType);
            // Set folderPath via reflection.
            FieldInfo folderField = openVatEditorType.GetField("folderPath",
                BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            if (folderField != null) folderField.SetValue(instance, folder);

            MethodInfo process = openVatEditorType.GetMethod("ProcessOpenVATContent",
                BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            if (process == null)
            {
                throw new Exception("OpenVATEditor.ProcessOpenVATContent method not found");
            }
            process.Invoke(instance, null);
            ScriptableObject.DestroyImmediate(instance);
            AssetDatabase.SaveAssets();
            Debug.Log("CLISetupURPAndBuild: ran OpenVATEditor.ProcessOpenVATContent");

            // OpenVATEditor assigns `Renderer.sharedMaterial` (singular)
            // to the generated prefab, which only paints submesh 0. For
            // a multi-submesh source like the Rumba dancer (11 material
            // slots: Skin/Clothes/Eyes/Cigar etc.) the other 10 slots
            // stay null and render with Unity's missing-material pink.
            // Fix by walking the prefab and replicating the VAT
            // material across every slot.
            FixupMultiSubmeshPrefab(folder);
        }

        // Construct a simple URP scene that uses the prefab OpenVAT
        // created at `Assets/OpenVATContent/<model>.prefab`.
        static void BuildScene()
        {
            var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);

            // Camera.
            var camGo = new GameObject("Main Camera");
            var cam   = camGo.AddComponent<Camera>();
            camGo.AddComponent<AudioListener>();
            camGo.tag = "MainCamera";
            cam.backgroundColor = new Color(0.45f, 0.55f, 0.65f, 1f);
            cam.fieldOfView = 45f;
            cam.farClipPlane = 100f;
            camGo.transform.position = new Vector3(0f, 1.5f, 3.5f);
            camGo.transform.rotation = Quaternion.Euler(-5f, 180f, 0f);

            // Light.
            var lightGo = new GameObject("Directional Light");
            var light = lightGo.AddComponent<Light>();
            light.type      = LightType.Directional;
            light.intensity = 1.0f;
            lightGo.transform.rotation = Quaternion.Euler(50f, -30f, 0f);

            // Spawn the OpenVAT-generated prefab.
            const string prefabPath = "Assets/OpenVATContent/rumba.prefab";
            var prefab = AssetDatabase.LoadAssetAtPath<GameObject>(prefabPath);
            if (prefab == null)
            {
                Debug.LogWarning($"CLISetupURPAndBuild: prefab {prefabPath} not found — OpenVAT processor may have used a different basename");
            }
            else
            {
                var instance = (GameObject)PrefabUtility.InstantiatePrefab(prefab);
                instance.transform.position = Vector3.zero;
            }

            const string scenePath = "Assets/Scenes/OpenVATWeb.unity";
            Directory.CreateDirectory("Assets/Scenes");
            EditorSceneManager.SaveScene(scene, scenePath);

            EditorBuildSettings.scenes = new[]
            {
                new EditorBuildSettingsScene(scenePath, enabled: true),
            };
            AssetDatabase.SaveAssets();
            Debug.Log($"CLISetupURPAndBuild: built scene {scenePath}");
        }

        static void BuildPlayer()
        {
            string outputDir = Path.Combine(Application.dataPath, "..", "Build", "Mac");
            Directory.CreateDirectory(outputDir);
            string exe = Path.Combine(outputDir, "UnityVATDemo.app");
            var scenes = EditorBuildSettings.scenes.Where(s => s.enabled).Select(s => s.path).ToArray();
            var opts = new BuildPlayerOptions
            {
                scenes           = scenes,
                locationPathName = exe,
                target           = BuildTarget.StandaloneOSX,
                options          = BuildOptions.None,
            };
            var report = BuildPipeline.BuildPlayer(opts);
            bool ok = report.summary.result == UnityEditor.Build.Reporting.BuildResult.Succeeded;
            Debug.Log($"CLISetupURPAndBuild: build {(ok ? "SUCCEEDED" : "FAILED")} ({report.summary.totalSize} bytes)");
            if (!ok) throw new Exception("BuildPipeline.BuildPlayer failed");
        }

        // Walk every prefab + every loose mesh asset OpenVATEditor
        // produced and make sure the VAT material covers all submesh
        // slots — not just slot 0.
        static void FixupMultiSubmeshPrefab(string folder)
        {
            // Find the material OpenVAT created (named *_mat.mat).
            var matGuids = AssetDatabase.FindAssets("t:Material", new[] { folder });
            Material vatMat = null;
            foreach (var g in matGuids)
            {
                var path = AssetDatabase.GUIDToAssetPath(g);
                if (path.EndsWith("_mat.mat"))
                {
                    vatMat = AssetDatabase.LoadAssetAtPath<Material>(path);
                    if (vatMat != null) break;
                }
            }
            if (vatMat == null)
            {
                Debug.LogWarning("FixupMultiSubmeshPrefab: no *_mat.mat found in " + folder);
                return;
            }

            // Walk every prefab in the folder.
            var prefabGuids = AssetDatabase.FindAssets("t:Prefab", new[] { folder });
            foreach (var g in prefabGuids)
            {
                var prefabPath = AssetDatabase.GUIDToAssetPath(g);
                var prefab = AssetDatabase.LoadAssetAtPath<GameObject>(prefabPath);
                if (prefab == null) continue;

                bool changed = false;
                foreach (var renderer in prefab.GetComponentsInChildren<Renderer>(true))
                {
                    Mesh m = null;
                    var mf = renderer.GetComponent<MeshFilter>();
                    if (mf != null) m = mf.sharedMesh;
                    if (m == null && renderer is SkinnedMeshRenderer smr) m = smr.sharedMesh;
                    if (m == null) continue;

                    int subCount = m.subMeshCount;
                    if (renderer.sharedMaterials.Length >= subCount &&
                        renderer.sharedMaterials.All(x => x != null)) continue;

                    var arr = new Material[subCount];
                    for (int i = 0; i < subCount; i++) arr[i] = vatMat;
                    renderer.sharedMaterials = arr;
                    changed = true;
                }
                if (changed)
                {
                    PrefabUtility.SavePrefabAsset(prefab);
                    Debug.Log($"FixupMultiSubmeshPrefab: replicated VAT material across all submesh slots on {prefabPath}");
                }
            }
            AssetDatabase.SaveAssets();
        }

        static Type[] SafeGetTypes(Assembly a)
        {
            try   { return a.GetTypes(); }
            catch { return Array.Empty<Type>(); }
        }
    }
}
#endif
