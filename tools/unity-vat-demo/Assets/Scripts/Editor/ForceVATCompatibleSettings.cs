// ForceVATCompatibleSettings — clamps VertexChannelCompressionMask
// to a VAT-safe value on every editor launch. The user's bake stores
// per-vertex bake-column indices in UV1 with values up to 5827; Unity's
// default mask (4054) has bit 4 set, which causes UV1 to be uploaded
// to the GPU as FP16. FP16 quantizes integer values above ~1024 and
// turns the dancer into a fan-of-triangles below that boundary.
//
// We force the mask via the documented PlayerSettings API every time
// the editor reloads the assemblies — that's the only point at which
// Unity re-reads ProjectSettings.asset, and any hand-edit to the file
// gets clobbered when the editor saves its in-memory copy back to
// disk. Doing it in code means our preferred value wins on first
// open and on every domain reload.

#if UNITY_EDITOR
using UnityEditor;
using UnityEngine;

namespace QtMeshEditor.VAT.Editor
{
    [InitializeOnLoad]
    static class ForceVATCompatibleSettings
    {
        // 3974 = bit pattern with UV0/1/2/3 compression bits cleared.
        // Unity 6 marked `vertexChannelCompressionMask` `internal` — it's
        // still wired through to ProjectSettings.asset but no longer
        // public API. Reach it via reflection; the alternative is a
        // SerializedObject read of ProjectSettings.asset, which is even
        // more fragile because the field path could move between Unity
        // versions. If the property genuinely disappears in a future
        // editor, the catch logs a warning and falls back to no-op —
        // the .meta-written value in ProjectSettings.asset is still the
        // initial seed for fresh clones.
        const int kVATCompatibleMask = 3974;

        static ForceVATCompatibleSettings()
        {
            try
            {
                var prop = typeof(PlayerSettings).GetProperty(
                    "vertexChannelCompressionMask",
                    System.Reflection.BindingFlags.Public
                    | System.Reflection.BindingFlags.NonPublic
                    | System.Reflection.BindingFlags.Static);
                if (prop == null)
                {
                    Debug.LogWarning("ForceVATCompatibleSettings: PlayerSettings.vertexChannelCompressionMask not found; rely on ProjectSettings.asset's checked-in value.");
                    return;
                }
                int current = (int)prop.GetValue(null);
                if (current != kVATCompatibleMask)
                {
                    prop.SetValue(null, kVATCompatibleMask);
                    AssetDatabase.SaveAssets();
                    Debug.Log(
                        "ForceVATCompatibleSettings: clamped VertexChannelCompressionMask " +
                        $"from {current} to {kVATCompatibleMask} (UV channels uncompressed) " +
                        "so VAT column indices in UV1 survive the GPU upload.");
                }
            }
            catch (System.Exception e)
            {
                Debug.LogWarning($"ForceVATCompatibleSettings: reflection access failed: {e.Message}");
            }
        }
    }
}
#endif
