// BootstrapVAT — auto-wires a VATPlayer's bake fields by reading the
// sidecar JSON when the component is added in the editor. Lives in an
// `Editor/` folder so it's stripped from runtime builds.
//
// On a fresh `Component → Add Component → VAT Player` it tries to find
// `Assets/VAT/Rumba/mixamo.com-remap_info.json` and parse Frames + Min +
// Max out of it. If the project ships only the Rumba bake this is
// enough to make the demo run with zero extra clicks.

#if UNITY_EDITOR
using System.IO;
using UnityEditor;
using UnityEngine;

namespace QtMeshEditor.VAT.Editor
{
    [InitializeOnLoad]
    static class BootstrapVAT
    {
        // Default bake folder relative to the Assets root. Hard-coded
        // because the demo project ships exactly one bake.
        const string kBakeDir        = "Assets/VAT/Rumba";
        const string kSidecarName    = "mixamo.com-remap_info.json";
        const string kPositionTexName = "mixamo.com_pos.png";
        const string kDiffuseTexName  = "Boss_diffuse.png";
        // We probe OBJ first because Unity 6's stock importer handles
        // .obj natively, and `qtmesh`'s custom FBX writer produces
        // 7300 binaries that Autodesk's strict FBX SDK (which Unity
        // uses) rejects as corrupt. `.gltf` needs the UnityGLTF
        // package — kept as a fallback for users who installed it.
        // The bake's vertex column order is preserved across formats
        // since `qtmesh convert` re-exports through the same Ogre
        // intermediate the baker walked.
        static readonly string[] kSourceMeshCandidates = {
            "source.obj",
            "source.gltf",
        };

        static BootstrapVAT()
        {
            ObjectFactory.componentWasAdded -= OnComponentAdded;
            ObjectFactory.componentWasAdded += OnComponentAdded;
        }

        static void OnComponentAdded(Component c)
        {
            // Fire-on-add is best-effort. The bake's AssetDatabase entries
            // may not exist yet on first project open (Unity is still
            // importing the FBX), in which case AutoWire silently leaves
            // empty slots empty — the inspector button is the user's
            // explicit follow-up.
            if (c is VATPlayer p) AutoWire(p, verbose: false);
        }

        /// <summary>Re-populate the VATPlayer's bake fields from
        /// `Assets/VAT/Rumba/`. Slots that the user has already filled
        /// in are preserved; slots that are null get auto-filled. Frame
        /// count + bounds are always overwritten from the sidecar (the
        /// sidecar is the source of truth for those — typing them by
        /// hand only invites desync).</summary>
        /// <param name="verbose">When true, log even when auto-wire
        /// could only partially populate the slots (the inspector
        /// button passes true; the auto-fire on add passes false to
        /// keep the console quiet during normal use).</param>
        public static void AutoWire(VATPlayer p, bool verbose)
        {
            string sidecarPath = $"{kBakeDir}/{kSidecarName}";
            if (!File.Exists(sidecarPath))
            {
                if (verbose) Debug.LogWarning($"BootstrapVAT: sidecar not found at {sidecarPath}.");
                return;
            }

            // Parse the JSON sidecar. Unity's JsonUtility doesn't handle
            // string-encoded floats in arrays, so we do a tiny hand-roll.
            // The schema is small and known.
            string raw = File.ReadAllText(sidecarPath);
            if (!TryParseSidecar(raw, out int frames, out Vector3 mn, out Vector3 mx))
            {
                Debug.LogWarning($"BootstrapVAT: couldn't parse {sidecarPath}; leaving VATPlayer at defaults.");
                return;
            }

            // Skip auto-wire on slots the user has already populated —
            // they presumably know what they're doing.
            if (p.positionTexture == null)
                p.positionTexture = AssetDatabase.LoadAssetAtPath<Texture2D>($"{kBakeDir}/{kPositionTexName}");
            if (p.diffuseTexture == null)
                p.diffuseTexture  = AssetDatabase.LoadAssetAtPath<Texture2D>($"{kBakeDir}/{kDiffuseTexName}");
            if (p.sourceMesh == null)
            {
                foreach (var name in kSourceMeshCandidates)
                {
                    p.sourceMesh = FindFirstMeshInModel($"{kBakeDir}/{name}");
                    if (p.sourceMesh != null) break;
                }
            }
            p.frameCount = frames;
            p.boundsMin  = mn;
            p.boundsMax  = mx;

            EditorUtility.SetDirty(p);
            if (verbose || p.sourceMesh != null)
            {
                string meshName = p.sourceMesh != null ? p.sourceMesh.name : "<not found — re-run after Unity finishes importing the FBX>";
                Debug.Log($"BootstrapVAT: auto-wired VATPlayer from {sidecarPath} (frames={frames}, bounds=[{mn} .. {mx}], mesh={meshName}).");
            }
        }

        // ── Mesh sub-asset lookup ────────────────────────────────────
        // .fbx (and .gltf with UnityGLTF) imports as a Model: a top-level
        // GameObject prefab with every Mesh stored as a sub-asset of the
        // importer. Reaching them via GetComponentInChildren<>() depends
        // on the import settings — for skinned meshes the importer may
        // strip the SkinnedMeshRenderer down to a static MeshFilter, in
        // which case the first lookup misses. Enumerating sub-assets
        // directly is robust against that variability.
        static Mesh FindFirstMeshInModel(string assetPath)
        {
            var subAssets = AssetDatabase.LoadAllAssetsAtPath(assetPath);
            if (subAssets == null) return null;
            foreach (var a in subAssets)
            {
                if (a is Mesh m) return m;
            }
            return null;
        }

        // ── Parser ──────────────────────────────────────────────────
        // The schema we accept is exactly what `qtmesh vat` writes today:
        //   { "os-remap": { "Frames": <int>, "Min": [str,str,str], "Max": [str,str,str] } }
        // String-encoded floats are how openvat's reference Python writes
        // them; we accept both quoted strings and raw numbers so future
        // bakes that store doubles directly still load.
        static bool TryParseSidecar(string json, out int frames, out Vector3 mn, out Vector3 mx)
        {
            frames = 0; mn = Vector3.zero; mx = Vector3.zero;
            try
            {
                int remapIdx = json.IndexOf("\"os-remap\"");
                if (remapIdx < 0) return false;

                frames = ParseIntField(json, remapIdx, "Frames");
                mn     = ParseVec3Field(json, remapIdx, "Min");
                mx     = ParseVec3Field(json, remapIdx, "Max");
                return frames > 0;
            }
            catch { return false; }
        }

        static int ParseIntField(string s, int start, string key)
        {
            int k = s.IndexOf("\"" + key + "\"", start);
            if (k < 0) return 0;
            int colon = s.IndexOf(':', k);
            int end   = s.IndexOfAny(new[] { ',', '}', '\n' }, colon + 1);
            return int.TryParse(s.Substring(colon + 1, end - colon - 1).Trim(), out int v) ? v : 0;
        }

        static Vector3 ParseVec3Field(string s, int start, string key)
        {
            int k = s.IndexOf("\"" + key + "\"", start);
            if (k < 0) return Vector3.zero;
            int open  = s.IndexOf('[', k);
            int close = s.IndexOf(']', open);
            if (open < 0 || close < 0) return Vector3.zero;
            string body = s.Substring(open + 1, close - open - 1);
            // The three numbers may or may not be quoted — strip both.
            body = body.Replace("\"", "");
            var parts = body.Split(',');
            float a = parts.Length > 0 ? float.Parse(parts[0].Trim(), System.Globalization.CultureInfo.InvariantCulture) : 0f;
            float b = parts.Length > 1 ? float.Parse(parts[1].Trim(), System.Globalization.CultureInfo.InvariantCulture) : 0f;
            float c = parts.Length > 2 ? float.Parse(parts[2].Trim(), System.Globalization.CultureInfo.InvariantCulture) : 0f;
            return new Vector3(a, b, c);
        }
    }
}
#endif
