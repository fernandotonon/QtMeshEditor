// CLIBuildScenes — programmatically constructs the demo scenes
// (Web.unity, PerfVAT.unity, PerfSkeleton.unity) from C# code,
// saves them under Assets/Scenes/, and adds them to Build Settings.
//
// Hand-authored .unity YAML is brittle because it cross-references
// every asset by .meta GUID. Constructing the scene in code via
// `EditorSceneManager` + the documented Unity API lets us produce
// reproducible scenes that work on every machine without GUID
// surgery. The CLIBuilder picks them up automatically after this
// script runs.
//
// Usage from CLI:
//   /Applications/Unity/Hub/Editor/<ver>/Unity.app/Contents/MacOS/Unity \
//     -batchmode -nographics -quit \
//     -projectPath tools/unity-vat-demo \
//     -executeMethod QtMeshEditor.VAT.Editor.CLIBuildScenes.BuildAll

#if UNITY_EDITOR
using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace QtMeshEditor.VAT.Editor
{
    public static class CLIBuildScenes
    {
        const string kScenesDir = "Assets/Scenes";

        // CLI entry point. Builds all three demo scenes + wires them
        // into Build Settings in deterministic order.
        public static void BuildAll()
        {
            Directory.CreateDirectory(kScenesDir);

            // Force-reimport the bake assets so the VATAssetPostprocessor
            // + QtmGltfImporter run with the latest settings every time
            // we CI-build. Without this, Unity uses the cached import
            // results and changes to importer settings don't apply
            // until the user deletes Library/.
            foreach (var path in new[]
            {
                "Assets/VAT/Rumba/source.gltf",
                "Assets/VAT/Rumba/mixamo.com_pos.png",
                "Assets/VAT/Rumba/Boss_diffuse.png",
            })
            {
                AssetDatabase.ImportAsset(path,
                    ImportAssetOptions.ForceUpdate | ImportAssetOptions.ForceSynchronousImport);
            }

            var webScene  = BuildWeb();
            var perfScene = BuildPerfVAT();
            var perfSkeletonPath = $"{kScenesDir}/PerfSkeleton.unity";

            // Register in Build Settings — first scene is the boot
            // scene the runtime opens at Play. Preserve PerfSkeleton if
            // a separate workflow has already produced it so we don't
            // drop it from the build settings.
            var scenes = new List<EditorBuildSettingsScene>
            {
                new EditorBuildSettingsScene(webScene,  enabled: true),
                new EditorBuildSettingsScene(perfScene, enabled: true),
            };
            if (File.Exists(perfSkeletonPath))
                scenes.Add(new EditorBuildSettingsScene(perfSkeletonPath, enabled: true));
            EditorBuildSettings.scenes = scenes.ToArray();
            AssetDatabase.SaveAssets();
            Debug.Log($"CLIBuildScenes: built {kScenesDir}/Web.unity + {kScenesDir}/PerfVAT.unity" +
                      (File.Exists(perfSkeletonPath) ? $" + {kScenesDir}/PerfSkeleton.unity" : ""));
        }

        static string BuildWeb()
        {
            var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);

            // Light.
            var lightGo = new GameObject("Directional Light");
            var light = lightGo.AddComponent<Light>();
            light.type = LightType.Directional;
            light.intensity = 1.2f;
            lightGo.transform.rotation = Quaternion.Euler(50, -30, 0);

            // Camera (target = (0,1,0), 4.5 m back).
            var camGo = new GameObject("Main Camera");
            camGo.AddComponent<Camera>();
            camGo.AddComponent<AudioListener>();
            camGo.tag = "MainCamera";
            var orbit = camGo.AddComponent<OrbitCamera>();
            orbit.target   = new Vector3(0, 1f, 0);
            orbit.distance = 4.5f;
            orbit.yawDeg   = 15f;
            orbit.pitchDeg = -10f;

            // VAT dancer GameObject — placed at origin so the orbit
            // target lands on its torso.
            var dancer = new GameObject("VAT Dancer");
            dancer.AddComponent<MeshFilter>();
            dancer.AddComponent<MeshRenderer>();
            var player = dancer.AddComponent<VATPlayer>();
            // Auto-wire from the bake (reads sidecar JSON).
            BootstrapVAT.AutoWire(player, verbose: true);
            // Diagnostic mode: bypass VAT replay and render the bind
            // pose, so we can confirm the mesh import is correct
            // independently of the VAT path.
            player.bypassVAT = true;

            string path = $"{kScenesDir}/Web.unity";
            EditorSceneManager.SaveScene(scene, path);
            return path;
        }

        static string BuildPerfVAT()
        {
            var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);

            // Light (shadows OFF for apples-to-apples perf measurement
            // against PerfSkeleton — the shadow pass would otherwise
            // dominate the cost).
            var lightGo = new GameObject("Directional Light");
            var light = lightGo.AddComponent<Light>();
            light.type = LightType.Directional;
            light.intensity = 1.2f;
            light.shadows = LightShadows.None;
            lightGo.transform.rotation = Quaternion.Euler(50, -30, 0);

            // Camera high enough to frame a 32×32 grid (spacing 1.6).
            var camGo = new GameObject("Main Camera");
            var cam   = camGo.AddComponent<Camera>();
            camGo.AddComponent<AudioListener>();
            camGo.tag = "MainCamera";
            cam.fieldOfView = 60f;
            var orbit = camGo.AddComponent<OrbitCamera>();
            orbit.target   = new Vector3(0, 1f, 25f);
            orbit.distance = 35f;
            orbit.yawDeg   = 0f;
            orbit.pitchDeg = -25f;
            orbit.maxDistance = 120f;

            // Spawner — the spawner reads the bake fields by hand
            // because PerfSpawnerVAT predates the BootstrapVAT helper.
            var spawner = new GameObject("PerfSpawner");
            var ps = spawner.AddComponent<PerfSpawnerVAT>();
            // Find the bake assets by AssetDatabase path so the same
            // settings the bake sidecar carries are reused here.
            ps.sourceMesh      = FindBakeMesh();
            ps.positionTexture = AssetDatabase.LoadAssetAtPath<Texture2D>("Assets/VAT/Rumba/mixamo.com_pos.png");
            ps.diffuseTexture  = AssetDatabase.LoadAssetAtPath<Texture2D>("Assets/VAT/Rumba/Boss_diffuse.png");
            // Fail fast if any required asset is missing — saving the
            // scene with nulls would emit a "successful" build that
            // crashes at runtime. List the specific gap so the user can
            // fix the bake.
            if (ps.sourceMesh == null || ps.positionTexture == null || ps.diffuseTexture == null)
            {
                var missing = new List<string>();
                if (ps.sourceMesh == null)      missing.Add("sourceMesh (Assets/VAT/Rumba/source.gltf)");
                if (ps.positionTexture == null) missing.Add("positionTexture (Assets/VAT/Rumba/mixamo.com_pos.png)");
                if (ps.diffuseTexture == null)  missing.Add("diffuseTexture (Assets/VAT/Rumba/Boss_diffuse.png)");
                throw new InvalidOperationException(
                    "CLIBuildScenes: required VAT assets are missing — cannot build PerfVAT scene: " +
                    string.Join(", ", missing));
            }
            // The Rumba bake's sidecar values — hard-coded here because
            // the spawner constructs all instances at Start() and
            // doesn't read the JSON itself.
            ps.frameCount = 71;
            ps.boundsMin  = new Vector3(-0.7f, -0.1f, -0.6f);
            ps.boundsMax  = new Vector3( 0.6f,  2.0f,  0.4f);
            ps.gridSize   = 32;
            ps.spacing    = 1.6f;

            // FPS overlay.
            var ui = new GameObject("FPSOverlay");
            ui.AddComponent<FPSOverlay>();

            string path = $"{kScenesDir}/PerfVAT.unity";
            EditorSceneManager.SaveScene(scene, path);
            return path;
        }

        // The custom QtmGltfImporter produces a Mesh sub-asset of the
        // .gltf with the file's basename — find it by scanning sub-assets.
        static Mesh FindBakeMesh()
        {
            const string gltfPath = "Assets/VAT/Rumba/source.gltf";
            foreach (var a in AssetDatabase.LoadAllAssetsAtPath(gltfPath))
                if (a is Mesh m) return m;
            Debug.LogError($"CLIBuildScenes: no Mesh sub-asset at {gltfPath} — re-import the .gltf");
            return null;
        }
    }
}
#endif
