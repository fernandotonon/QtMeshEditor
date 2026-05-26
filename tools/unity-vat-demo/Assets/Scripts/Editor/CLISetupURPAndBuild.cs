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
                ForceOpenGLCoreOnMac();
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

        // Force the macOS Standalone target to use OpenGLCore instead
        // of Metal. Unity 6's Metal backend doesn't bind textures to
        // the vertex stage on Apple Silicon — every vertex texture
        // fetch returns the same texel regardless of input UV, which
        // breaks VAT replay. OpenGL's vertex shaders handle the same
        // SAMPLE_TEXTURE2D_LOD calls correctly.
        //
        // The setting is `PlayerSettings.SetGraphicsAPIs(target, apis[])`
        // + `SetUseDefaultGraphicsAPIs(target, false)` to opt out of
        // Unity's "let the platform decide" path.
        static void ForceOpenGLCoreOnMac()
        {
            PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.StandaloneOSX, false);
            PlayerSettings.SetGraphicsAPIs(BuildTarget.StandaloneOSX,
                new[] { UnityEngine.Rendering.GraphicsDeviceType.OpenGLCore });
            AssetDatabase.SaveAssets();
            Debug.Log("CLISetupURPAndBuild: set StandaloneOSX graphics API to OpenGLCore (workaround for Metal vertex-stage texture fetch bug)");
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

            // Force-reimport the position texture FIRST so the
            // VATAssetPostprocessor applies maxTextureSize=8192 +
            // RGBA64. Without this, the texture downscales to 64px tall
            // and OpenVATEditor records that as `_resolutionY` on the
            // material — the shader then samples the wrong rows and
            // playback dies.
            foreach (var f in Directory.GetFiles(folder, "*_vat.png"))
            {
                string assetPath = f.Replace("\\", "/");
                AssetDatabase.ImportAsset(assetPath,
                    ImportAssetOptions.ForceUpdate | ImportAssetOptions.ForceSynchronousImport);
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

            // Swap the material's shader from the broken
            // `Shader Graphs/openVAT_decoder` (whose
            // VertexDescription.Position block is not wired to the
            // texture-sampling subgraph in the upstream package — see
            // README's Status section) to our custom HLSL shader that
            // does the vertex deformation correctly.
            SwapToCustomHlslShader(folder);
            // OpenVATEditor assigns `Renderer.sharedMaterial` (singular)
            // to the generated prefab, which only paints submesh 0. For
            // a multi-submesh source like the Rumba dancer (11 material
            // slots: Skin/Clothes/Eyes/Cigar etc.) the other 10 slots
            // stay null and render with Unity's missing-material pink.
            // Fix by walking the prefab and replicating the VAT
            // material across every slot.
            FixupMultiSubmeshPrefab(folder);
            // Two more material fixups OpenVATEditor leaves us with:
            //   - `_speed = 0` — the shader's default is 1.0 but
            //     CreateMaterial seeds it to 0, so without us setting
            //     it the dancer stays at frame 0 forever.
            //   - `_resolutionY = 64` (or whatever Unity decided on first
            //     import). The position texture is 5828×142; if the
            //     texture importer hadn't finished the maxTextureSize=8192
            //     resize before OpenVATEditor read `.height`, the material
            //     gets a stale value. Set it directly from the asset on
            //     disk (PNG header is authoritative).
            FixupAnimationUniforms(folder);
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
                // Unpack the prefab connection so AddComponent + the new
                // values actually serialize into the scene rather than
                // staying on the prefab override. Without this, the
                // OpenVATDriver component drops on scene reopen.
                PrefabUtility.UnpackPrefabInstance(instance,
                    PrefabUnpackMode.Completely, InteractionMode.AutomatedAction);
                // Attach an OpenVATDriver that ticks `_frame` per
                // Update — the shadergraph's `_Time`-driven path doesn't
                // animate reliably in the standalone player on Unity 6.
                // Driving _frame from C# guarantees playback.
                var driver = instance.AddComponent<OpenVATDriver>();
                driver.fps = 30f;
                driver.frames = 71;
                Debug.Log("CLISetupURPAndBuild: attached OpenVATDriver to scene instance, unpacked prefab connection");
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

        // Swap the OpenVATEditor-assigned shader for our custom HLSL one
        // that actually wires the vertex deformation. The upstream graph
        // declares all the same uniforms (so texture + bounds + frame
        // count carry over) but leaves VertexDescription.Position
        // disconnected.
        static void SwapToCustomHlslShader(string folder)
        {
            var newShader = Shader.Find("QtMeshEditor/OpenVAT_URP");
            if (newShader == null)
            {
                Debug.LogWarning("SwapToCustomHlslShader: QtMeshEditor/OpenVAT_URP shader not found in project");
                return;
            }
            var matGuids = AssetDatabase.FindAssets("t:Material", new[] { folder });
            foreach (var g in matGuids)
            {
                var path = AssetDatabase.GUIDToAssetPath(g);
                if (!path.EndsWith("_mat.mat")) continue;
                var mat = AssetDatabase.LoadAssetAtPath<Material>(path);
                if (mat == null) continue;
                mat.shader = newShader;
                EditorUtility.SetDirty(mat);
                Debug.Log($"SwapToCustomHlslShader: swapped {path} → QtMeshEditor/OpenVAT_URP");
            }
            AssetDatabase.SaveAssets();
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

        // Patches `_speed` and `_resolutionY` on every *_mat.mat file
        // OpenVATEditor produced under `folder`. Without this the dancer
        // stays static (speed=0) and the frame-row calculation samples
        // the wrong rows (resolutionY=64 vs the actual 142).
        static void FixupAnimationUniforms(string folder)
        {
            var matGuids = AssetDatabase.FindAssets("t:Material", new[] { folder });
            foreach (var g in matGuids)
            {
                var path = AssetDatabase.GUIDToAssetPath(g);
                if (!path.EndsWith("_mat.mat")) continue;
                var mat = AssetDatabase.LoadAssetAtPath<Material>(path);
                if (mat == null) continue;

                bool dirty = false;
                if (Mathf.Abs(mat.GetFloat("_speed")) < 0.001f)
                {
                    mat.SetFloat("_speed", 1f);
                    dirty = true;
                }
                // _UseTime defaults to 1 in the shadergraph properties,
                // but materials created via `new Material(shader)` end
                // up with the toggle at 0 (Unity initialises boolean
                // toggles to false regardless of the property default).
                // With _UseTime=0 the shader reads `_frame` instead of
                // `_Time.y * _speed * _frames` → dancer freezes at frame 0.
                // OpenVAT's shader graph declares the property as
                // `_UseTIme` (typo: capital T followed by lowercase im
                // followed by capital m, total 6 chars). Unity 6
                // normalizes the property name when stamping it on
                // a Material to `_UseTime` (standard CamelCase). We
                // set BOTH spellings — only one will hit, but neither
                // throws. Same for the keyword.
                if (mat.HasProperty("_UseTIme") && mat.GetFloat("_UseTIme") < 0.5f)
                {
                    mat.SetFloat("_UseTIme", 1f);
                    dirty = true;
                }
                if (mat.HasProperty("_UseTime") && mat.GetFloat("_UseTime") < 0.5f)
                {
                    mat.SetFloat("_UseTime", 1f);
                    dirty = true;
                }
                mat.EnableKeyword("_USETIME_ON");
                mat.EnableKeyword("_USETIME_ON".Replace("USETIME", "USETIME"));   // no-op alt spelling
                // Diagnostic dump of every property the shader exposes
                // and its current value, so we know what the build will
                // actually see.
                int propCount = mat.shader.GetPropertyCount();
                var props = new System.Text.StringBuilder();
                for (int i = 0; i < propCount; i++)
                {
                    string pname = mat.shader.GetPropertyName(i);
                    var ptype = mat.shader.GetPropertyType(i);
                    string val;
                    try
                    {
                        val = ptype switch
                        {
                            UnityEngine.Rendering.ShaderPropertyType.Float => mat.GetFloat(pname).ToString("F3"),
                            UnityEngine.Rendering.ShaderPropertyType.Range => mat.GetFloat(pname).ToString("F3"),
                            UnityEngine.Rendering.ShaderPropertyType.Vector => mat.GetVector(pname).ToString(),
                            UnityEngine.Rendering.ShaderPropertyType.Color => mat.GetColor(pname).ToString(),
                            UnityEngine.Rendering.ShaderPropertyType.Texture => mat.GetTexture(pname) != null ? "<tex>" : "null",
                            UnityEngine.Rendering.ShaderPropertyType.Int => mat.GetInt(pname).ToString(),
                            _ => "?",
                        };
                    }
                    catch (Exception) { val = "<err>"; }
                    props.Append($"\n    {pname} ({ptype}) = {val}");
                }
                Debug.Log($"FixupAnimationUniforms: shader '{mat.shader.name}' properties:{props}");
                // Same story for _UsePackedNormals — our bake packs the
                // normals into the bottom half of the texture (Frames..
                // 2*Frames rows). Setting this enables that decode path.
                if (mat.GetFloat("_UsePackedNormals") < 0.5f)
                {
                    mat.SetFloat("_UsePackedNormals", 1f);
                    mat.EnableKeyword("_USEPACKEDNORMALS_ON");
                    dirty = true;
                }
                // Read the position-texture's TRUE height directly from
                // the .png on disk via the AssetDatabase (the importer
                // settings now force maxTextureSize 8192, so this matches
                // the source 142).
                var posTex = mat.GetTexture("_openVAT_main") as Texture2D;
                if (posTex != null && Mathf.Abs(mat.GetFloat("_resolutionY") - posTex.height) > 0.5f)
                {
                    mat.SetFloat("_resolutionY", posTex.height);
                    dirty = true;
                }
                // _exaggeration multiplies the encoded vertex
                // displacement. Defaults to 0 on a freshly-created
                // material (and the shadergraph schema default is 0
                // too), which silently mutes ALL animation regardless
                // of how `_frame` / `_Time` ticks — the deformation
                // amplitude is zero, so verts always sit at bind pose.
                // 1.0 = "play back at the bake's natural amplitude".
                if (mat.HasProperty("_exaggeration") && Mathf.Abs(mat.GetFloat("_exaggeration")) < 0.001f)
                {
                    mat.SetFloat("_exaggeration", 1f);
                    dirty = true;
                }
                if (dirty)
                {
                    EditorUtility.SetDirty(mat);
                    Debug.Log($"FixupAnimationUniforms: patched {path} → _speed={mat.GetFloat("_speed")}, _resolutionY={mat.GetFloat("_resolutionY")}");
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
