// CLIBuilder — headless build entry point so the demo can be built
// from the repo root without opening the Unity editor GUI:
//
//   /Applications/Unity/Hub/Editor/<ver>/Unity.app/Contents/MacOS/Unity \
//     -batchmode -nographics -quit \
//     -projectPath tools/unity-vat-demo \
//     -executeMethod QtMeshEditor.VAT.Editor.CLIBuilder.BuildMac
//
// Builds a self-contained .app at tools/unity-vat-demo/Build/Mac/.
// We deliberately don't create scenes in this script — the user
// builds those interactively per the README (Unity scene .unity files
// are GUID-coupled to .meta files and are brittle to hand-author).
// What CLIBuilder does is provide a build entry point once those
// scenes exist; without scenes it just logs and returns.

#if UNITY_EDITOR
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace QtMeshEditor.VAT.Editor
{
    public static class CLIBuilder
    {
        public static void BuildMac()       => BuildFor(BuildTarget.StandaloneOSX,     "Mac");
        public static void BuildWindows()   => BuildFor(BuildTarget.StandaloneWindows64,"Windows");
        public static void BuildLinux()     => BuildFor(BuildTarget.StandaloneLinux64, "Linux");

        static void BuildFor(BuildTarget target, string subdir)
        {
            // Pick up every .unity scene the project ships with. The user
            // builds these by hand per the README; if none exist we log a
            // helpful message instead of failing the CI silently with an
            // empty .app.
            var scenes = new List<string>();
            foreach (var guid in AssetDatabase.FindAssets("t:Scene"))
            {
                var path = AssetDatabase.GUIDToAssetPath(guid);
                if (path.StartsWith("Assets/")) scenes.Add(path);
            }

            if (scenes.Count == 0)
            {
                Debug.LogWarning(
                    "CLIBuilder: no .unity scenes found under Assets/. " +
                    "Build the Web/PerfVAT/PerfSkeleton scenes per the README " +
                    "(File → New Scene → save under Assets/Scenes/) then re-run.");
                EditorApplication.Exit(0);
                return;
            }

            string outputDir = Path.Combine(Application.dataPath, "..", "Build", subdir);
            Directory.CreateDirectory(outputDir);

            string exe = target switch
            {
                BuildTarget.StandaloneOSX      => Path.Combine(outputDir, "UnityVATDemo.app"),
                BuildTarget.StandaloneWindows64=> Path.Combine(outputDir, "UnityVATDemo.exe"),
                BuildTarget.StandaloneLinux64  => Path.Combine(outputDir, "UnityVATDemo"),
                _                              => Path.Combine(outputDir, "UnityVATDemo")
            };

            var opts = new BuildPlayerOptions
            {
                scenes           = scenes.ToArray(),
                locationPathName = exe,
                target           = target,
                options          = BuildOptions.None,
            };

            Debug.Log($"CLIBuilder: building {scenes.Count} scene(s) → {exe}");
            BuildReport rep = BuildPipeline.BuildPlayer(opts);
            bool ok = rep.summary.result == BuildResult.Succeeded;
            Debug.Log($"CLIBuilder: build {(ok ? "SUCCEEDED" : "FAILED")} — " +
                      $"{rep.summary.totalSize} bytes, {rep.summary.totalErrors} errors.");
            EditorApplication.Exit(ok ? 0 : 1);
        }
    }
}
#endif
