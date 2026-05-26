// VATAssetPostprocessor — enforces the right import settings on the
// bake assets when Unity imports them, so the demo "just works" on
// first project open without the user having to hand-tweak each
// importer.
//
// Why a postprocessor and not just .meta hand-edits: Unity will
// silently rewrite parts of a Model importer's .meta when it
// reimports, sometimes resetting fields that don't survive a round-
// trip. A postprocessor runs every import and is the authoritative
// place to encode "this is what this asset needs to look like".
//
// What it enforces:
//   • Other model files (.fbx/.obj/.dae) under the bake folder, if any,
//                  get the same VAT-friendly settings as a safety net.
//                  The recommended path is the .gltf consumed by the
//                  custom QtmGltfImporter, which doesn't go through
//                  ModelImporter at all and so doesn't need any of
//                  these.
//   • mixamo.com_pos.png → sRGB OFF (positions are linear data),
//                          Compression None (any lossy compression
//                          corrupts the position values),
//                          Filter Mode Point (no bilinear blur between
//                          frame rows),
//                          Wrap Mode Clamp,
//                          MipMaps disabled (sampling mips of a
//                          packed-data texture is nonsense).

#if UNITY_EDITOR
using UnityEditor;
using UnityEngine;

namespace QtMeshEditor.VAT.Editor
{
    public class VATAssetPostprocessor : AssetPostprocessor
    {
        const string kBakeDir = "Assets/VAT/Rumba";

        void OnPreprocessModel()
        {
            if (!assetPath.StartsWith(kBakeDir)) return;

            var imp = (ModelImporter)assetImporter;
            bool dirty = false;

            if (!imp.isReadable)              { imp.isReadable = true;                                 dirty = true; }
            if (imp.animationType != ModelImporterAnimationType.None)
                                              { imp.animationType = ModelImporterAnimationType.None;   dirty = true; }
            if (imp.importAnimation)          { imp.importAnimation = false;                           dirty = true; }
            // The bake's VAT replaces skinning at runtime, so the
            // skeleton + skin data on the imported asset is dead weight.
            // Stripping it also makes the Mesh sub-asset surface as a
            // first-class MeshFilter source in the picker.
            if (imp.optimizeGameObjects)      { imp.optimizeGameObjects = false;                       dirty = true; }
            // Vertex order MUST be preserved. The VAT bake indexes
            // column N to the Nth vertex Unity rendered — if the
            // optimizer reorders verts, every column lookup goes to
            // the wrong sample and the dancer turns into noise.
            if (imp.meshOptimizationFlags != 0) { imp.meshOptimizationFlags = 0;                       dirty = true; }
            if (imp.weldVertices)             { imp.weldVertices = false;                              dirty = true; }

            if (dirty)
                Debug.Log($"VATAssetPostprocessor: normalized import settings on {assetPath} for VAT replay.");
        }

        void OnPreprocessTexture()
        {
            if (!assetPath.StartsWith(kBakeDir)) return;
            // Only retune the packed-data texture, not the diffuse —
            // the diffuse stays sRGB.
            if (!assetPath.EndsWith("_pos.png")) return;

            var imp = (TextureImporter)assetImporter;
            imp.sRGBTexture     = false;
            imp.textureCompression = TextureImporterCompression.Uncompressed;
            imp.filterMode      = FilterMode.Point;
            imp.wrapMode        = TextureWrapMode.Clamp;
            imp.mipmapEnabled   = false;
            // alphaSource = None — the bake has no alpha channel and
            // the sampler reads `.rgb` only.
            imp.alphaSource     = TextureImporterAlphaSource.None;
            // Max-size cap: the per-platform default is 2048, but the
            // Rumba bake is 5828 columns wide (and other bakes can be
            // larger). If left at 2048, Unity downscales the position
            // texture and every column index > 2047 reads the SAME
            // boundary column → mass collapse into fan-of-triangles.
            // 8192 covers the 5828-wide bake plus headroom for future
            // larger ones. Use NPOTScale.None so the (Width=5828,
            // Height=142) → does NOT get rounded to a power-of-two.
            imp.maxTextureSize  = 8192;
            imp.npotScale       = TextureImporterNPOTScale.None;
            // Override the per-platform setting too — the platform
            // override takes precedence over the default, and Unity
            // 6 sets a platform-specific 2048 cap on Standalone by
            // default.
            foreach (string platform in new[] { "Standalone", "WebGL", "iPhone", "Android" })
            {
                var settings = imp.GetPlatformTextureSettings(platform);
                if (settings.overridden || settings.maxTextureSize < 8192)
                {
                    settings.overridden     = true;
                    settings.maxTextureSize = 8192;
                    settings.textureCompression = TextureImporterCompression.Uncompressed;
                    imp.SetPlatformTextureSettings(settings);
                }
            }

            Debug.Log($"VATAssetPostprocessor: normalized texture settings on {assetPath} for VAT replay.");
        }
    }
}
#endif
