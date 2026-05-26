// QtmGltfImporter — minimal ScriptedImporter for .gltf files produced
// by `qtmesh vat`. The point of going custom (instead of e.g. the
// UnityGLTF or gltFast packages) is to PRESERVE VERTEX ORDER —
// the VAT bake indexes position-texture column N to the Nth vertex
// the baker walked, so any importer that welds, dedupes, or reorders
// verts under us breaks the column lookup.
//
// What it reads:
//   • Positions       (VEC3 float32 → Unity Vector3)
//   • Normals         (VEC3 float32 → Unity Vector3)  — optional
//   • TEXCOORD_0      (VEC2 float32 → Unity Vector2 uv)
//   • TEXCOORD_1      (VEC2 float32 → Unity Vector2 uv2) — carries the
//                     bake column index (qtmesh vat --emit-uv2)
//   • Indices         (SCALAR uint16 or uint32)
//
// What it ignores intentionally:
//   • Materials / textures (Unity has those already — we just need
//     geometry to drive VAT replay)
//   • Animations (the VAT texture replaces them)
//   • Skin / inverse-bind-matrices (VAT replaces skinning entirely)
//   • All glTF extensions
//
// Each glTF "primitive" becomes one Unity subMesh — preserves the
// per-submesh boundary so the renderer can assign one VAT material
// per slot. The big trick that makes the VAT replay work is that we
// DO NOT call mesh.Optimize() and DO NOT touch the vertex array
// after writing it — the order we set is the order the GPU sees.

#if UNITY_EDITOR
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using UnityEditor;
using UnityEditor.AssetImporters;
using UnityEngine;

namespace QtMeshEditor.VAT.Editor
{
    // Version bumps invalidate Unity's cached imports — bump when the
    // import-time data transformation changes (e.g. UV2 normalization
    // added in v2). Without this Unity reuses the previously-cached
    // Mesh asset and your importer code changes go unnoticed.
    [ScriptedImporter(version: 2, ext: "gltf")]
    public class QtmGltfImporter : ScriptedImporter
    {
        public override void OnImportAsset(AssetImportContext ctx)
        {
            try
            {
                ImportImpl(ctx);
            }
            catch (Exception e)
            {
                ctx.LogImportError($"QtmGltfImporter failed for {ctx.assetPath}: {e}");
            }
        }

        void ImportImpl(AssetImportContext ctx)
        {
            string jsonText = File.ReadAllText(ctx.assetPath);
            var doc = MiniJson.Deserialize(jsonText) as Dictionary<string, object>;
            if (doc == null) { ctx.LogImportError("glTF JSON parse failed"); return; }

            // ── Buffers (load all referenced .bin files) ────────────
            var buffers = new List<byte[]>();
            string gltfDir = Path.GetDirectoryName(ctx.assetPath);
            foreach (var b in AsList(doc, "buffers"))
            {
                var bm = (Dictionary<string, object>)b;
                string uri = bm.ContainsKey("uri") ? bm["uri"] as string : null;
                if (string.IsNullOrEmpty(uri))
                {
                    ctx.LogImportError("glTF buffer with no URI — embedded .glb buffers aren't supported by this importer");
                    return;
                }
                string binPath = Path.Combine(gltfDir, uri);
                ctx.DependsOnSourceAsset(binPath);
                buffers.Add(File.ReadAllBytes(binPath));
            }

            var bufferViews = AsList(doc, "bufferViews");
            var accessors   = AsList(doc, "accessors");
            var meshes      = AsList(doc, "meshes");

            if (meshes.Count == 0) { ctx.LogImportError("glTF has no meshes"); return; }

            // We merge ALL primitives across ALL meshes into a single Unity
            // Mesh asset (one subMesh per primitive). The Rumba bake walks
            // primitives in the same order the position texture's columns
            // were laid out — primitive 0 = columns 0..N0, primitive 1 =
            // columns N0..N0+N1, etc. — so concatenating in this order
            // matches the bake's column indexing.
            var allVerts   = new List<Vector3>();
            var allNormals = new List<Vector3>();
            var allUv0     = new List<Vector2>();
            var allUv1     = new List<Vector2>();
            var subMeshes  = new List<int[]>();   // one int[] per primitive
            int vertexBase = 0;

            foreach (var meshObj in meshes)
            {
                var meshDict = (Dictionary<string, object>)meshObj;
                var prims = AsList(meshDict, "primitives");
                foreach (var primObj in prims)
                {
                    var prim = (Dictionary<string, object>)primObj;
                    var attribs = (Dictionary<string, object>)prim["attributes"];

                    var posAcc = GetAccessor(accessors, attribs, "POSITION");
                    if (posAcc == null) { ctx.LogImportError("primitive missing POSITION"); return; }

                    var positions = ReadVec3(buffers, bufferViews, posAcc);
                    allVerts.AddRange(positions);

                    if (attribs.ContainsKey("NORMAL"))
                        allNormals.AddRange(ReadVec3(buffers, bufferViews,
                                                     GetAccessor(accessors, attribs, "NORMAL")));
                    else
                        for (int k = 0; k < positions.Count; k++) allNormals.Add(Vector3.up);

                    if (attribs.ContainsKey("TEXCOORD_0"))
                        allUv0.AddRange(ReadVec2(buffers, bufferViews,
                                                 GetAccessor(accessors, attribs, "TEXCOORD_0")));
                    else
                        for (int k = 0; k < positions.Count; k++) allUv0.Add(Vector2.zero);

                    if (attribs.ContainsKey("TEXCOORD_1"))
                        allUv1.AddRange(ReadVec2(buffers, bufferViews,
                                                 GetAccessor(accessors, attribs, "TEXCOORD_1")));
                    else
                        // No UV1: synthesise per-vertex column index (col, row=0)
                        // assuming positions are laid out left-to-right across
                        // the texture in one row. Only correct when there's a
                        // single primitive — multi-primitive bakes MUST ship
                        // an explicit TEXCOORD_1.
                        for (int k = 0; k < positions.Count; k++) allUv1.Add(new Vector2(vertexBase + k, 0));

                    // Mark that the uv1 values we just appended are
                    // raw integer column indices. We'll normalise
                    // them to [0,1] below, after we've concatenated
                    // every primitive's UV1 and know the maximum.

                    // Indices.
                    int[] indices;
                    if (prim.ContainsKey("indices"))
                    {
                        var idxAcc = (Dictionary<string, object>)accessors[(int)(long)prim["indices"]];
                        indices = ReadIndices(buffers, bufferViews, idxAcc);
                    }
                    else
                    {
                        // glTF spec allows non-indexed primitives; build a
                        // trivial 0..count-1 sequence.
                        indices = new int[positions.Count];
                        for (int i = 0; i < indices.Length; i++) indices[i] = i;
                    }

                    // No coordinate-system fix-up — the VAT shader overrides
                    // every vertex position via the texture sample, so what
                    // matters is (a) UV1 reaches the GPU with the correct
                    // column index, and (b) the texture sample lands at the
                    // bake-authored coordinate convention. Touching the
                    // vertex array here (X-negate, winding flip) is dead
                    // code for VAT replay and risks Unity recomputing
                    // tangents / bounds that obscure the actual issue.

                    // Rebase indices to the merged buffer (no winding flip).
                    var subIndices = new int[indices.Length];
                    for (int t = 0; t < indices.Length; t++)
                        subIndices[t] = indices[t] + vertexBase;
                    subMeshes.Add(subIndices);

                    vertexBase += positions.Count;
                }
            }

            // ── Build the Unity Mesh ─────────────────────────────────
            var mesh = new Mesh();
            mesh.name = Path.GetFileNameWithoutExtension(ctx.assetPath);
            if (allVerts.Count > 65535) mesh.indexFormat = UnityEngine.Rendering.IndexFormat.UInt32;

            // Force a Float32 vertex buffer layout BEFORE writing the
            // channels. Without this Unity's mesh asset cook can quantize
            // UV channels to FP16 even when VertexChannelCompressionMask
            // says otherwise — Unity 6 made the Player Setting an
            // internal hint rather than a hard contract, and the build
            // pipeline still applies per-channel compression based on
            // the SerializedData's declared format.
            //
            // For VAT replay UV1 carries integer column indices up to
            // 5827; FP16 has ~10 mantissa bits and snaps integers above
            // ~1024 to nearby multiples-of-2, turning the dancer into a
            // fan of triangles below the boundary (visible on PerfVAT
            // as a forest of red/black cones). The Float32 declaration
            // locks the channel at full precision in the cooked mesh.
            var layout = new[]
            {
                new UnityEngine.Rendering.VertexAttributeDescriptor(
                    UnityEngine.Rendering.VertexAttribute.Position,
                    UnityEngine.Rendering.VertexAttributeFormat.Float32, 3),
                new UnityEngine.Rendering.VertexAttributeDescriptor(
                    UnityEngine.Rendering.VertexAttribute.Normal,
                    UnityEngine.Rendering.VertexAttributeFormat.Float32, 3),
                new UnityEngine.Rendering.VertexAttributeDescriptor(
                    UnityEngine.Rendering.VertexAttribute.TexCoord0,
                    UnityEngine.Rendering.VertexAttributeFormat.Float32, 2),
                new UnityEngine.Rendering.VertexAttributeDescriptor(
                    UnityEngine.Rendering.VertexAttribute.TexCoord1,
                    UnityEngine.Rendering.VertexAttributeFormat.Float32, 2),
            };
            mesh.SetVertexBufferParams(allVerts.Count, layout);

            mesh.SetVertices(allVerts);
            mesh.SetNormals(allNormals);
            mesh.SetUVs(0, allUv0);

            // Normalize UV1 column indices to [0,1] to survive GPU FP16
            // vertex compression. URP under Metal quantizes UV channels
            // to half-float regardless of VertexChannelCompressionMask;
            // FP16 represents integers above ~1024 with progressively
            // worse precision (2048+ snaps to multiples of 2, 4096+ to
            // multiples of 4 etc.), turning the dancer's body into an
            // egg shape. [0,1] floats survive FP16 perfectly across the
            // full range, so we divide by texWidth at import time and
            // multiply back in the shader.
            float maxU = 0f;
            for (int i = 0; i < allUv1.Count; i++) if (allUv1[i].x > maxU) maxU = allUv1[i].x;
            // texWidth = maxU + 1 (since columns are 0..maxU). Round up
            // to a power-of-two for cleaner shader math; we tell the
            // shader the divisor via a Mesh tag (the shader reads it
            // from the position texture's actual width at runtime).
            if (maxU > 1.5f)   // only normalize when we have raw integer indices
            {
                float divisor = maxU + 1f;   // 5828 for the Rumba bake
                for (int i = 0; i < allUv1.Count; i++)
                    allUv1[i] = new Vector2(allUv1[i].x / divisor, allUv1[i].y);
                Debug.Log($"QtmGltfImporter: normalized UV1 by {divisor:F0} to fit FP16 precision (max raw was {maxU:F0})");
            }
            mesh.SetUVs(1, allUv1);

            mesh.subMeshCount = subMeshes.Count;
            for (int i = 0; i < subMeshes.Count; i++)
                mesh.SetIndices(subMeshes[i], MeshTopology.Triangles, i, calculateBounds: false);

            mesh.RecalculateBounds();
            // Deliberately NOT calling RecalculateNormals or
            // OptimizeIndexBuffers / Optimize — both would reorder verts
            // and silently invalidate the VAT column lookup.

            // Sanity-log so we can verify UV1 reached the mesh intact —
            // VAT replay completely depends on these column indices
            // surviving Unity's mesh upload to the GPU.
            float u1Min = float.MaxValue, u1Max = float.MinValue;
            float v1Min = float.MaxValue, v1Max = float.MinValue;
            foreach (var u in allUv1)
            {
                if (u.x < u1Min) u1Min = u.x;
                if (u.x > u1Max) u1Max = u.x;
                if (u.y < v1Min) v1Min = u.y;
                if (u.y > v1Max) v1Max = u.y;
            }
            Debug.Log($"QtmGltfImporter: imported {allVerts.Count} verts across " +
                      $"{subMeshes.Count} submeshes from {ctx.assetPath}. " +
                      $"UV1 range u=[{u1Min}..{u1Max}], v=[{v1Min}..{v1Max}] " +
                      $"(must be integer column indices in [0..texWidth-1] for VAT replay).");

            ctx.AddObjectToAsset("mesh", mesh);

            // Wrap in a prefab GameObject so users can drop the import
            // result into the scene like any other Model asset.
            var go = new GameObject(mesh.name);
            var mf = go.AddComponent<MeshFilter>();
            mf.sharedMesh = mesh;
            var mr = go.AddComponent<MeshRenderer>();
            // Material slots are intentionally left empty — VATPlayer
            // creates the VAT material at runtime and assigns it to all
            // submesh slots.
            mr.sharedMaterials = new Material[subMeshes.Count];

            ctx.AddObjectToAsset("prefab", go);
            ctx.SetMainObject(go);
        }

        // ── Accessor + buffer helpers ─────────────────────────────────

        static Dictionary<string, object> GetAccessor(List<object> accessors,
                                                     Dictionary<string, object> attribs,
                                                     string key)
        {
            if (!attribs.ContainsKey(key)) return null;
            int idx = (int)(long)attribs[key];
            return (Dictionary<string, object>)accessors[idx];
        }

        static List<Vector3> ReadVec3(List<byte[]> buffers,
                                      List<object> bufferViews,
                                      Dictionary<string, object> acc)
        {
            int count = (int)(long)acc["count"];
            var bytes = SliceAccessor(buffers, bufferViews, acc, count * 12);
            var result = new List<Vector3>(count);
            for (int i = 0; i < count; i++)
            {
                int o = i * 12;
                result.Add(new Vector3(
                    BitConverter.ToSingle(bytes, o + 0),
                    BitConverter.ToSingle(bytes, o + 4),
                    BitConverter.ToSingle(bytes, o + 8)));
            }
            return result;
        }

        static List<Vector2> ReadVec2(List<byte[]> buffers,
                                      List<object> bufferViews,
                                      Dictionary<string, object> acc)
        {
            int count = (int)(long)acc["count"];
            var bytes = SliceAccessor(buffers, bufferViews, acc, count * 8);
            var result = new List<Vector2>(count);
            for (int i = 0; i < count; i++)
            {
                int o = i * 8;
                result.Add(new Vector2(
                    BitConverter.ToSingle(bytes, o + 0),
                    BitConverter.ToSingle(bytes, o + 4)));
            }
            return result;
        }

        static int[] ReadIndices(List<byte[]> buffers,
                                 List<object> bufferViews,
                                 Dictionary<string, object> acc)
        {
            int count = (int)(long)acc["count"];
            int componentType = (int)(long)acc["componentType"];
            // 5121=UBYTE, 5123=USHORT, 5125=UINT
            int stride = componentType == 5121 ? 1 : (componentType == 5123 ? 2 : 4);
            var bytes = SliceAccessor(buffers, bufferViews, acc, count * stride);
            var result = new int[count];
            for (int i = 0; i < count; i++)
            {
                switch (componentType)
                {
                    case 5121: result[i] = bytes[i]; break;
                    case 5123: result[i] = BitConverter.ToUInt16(bytes, i * 2); break;
                    case 5125: result[i] = (int)BitConverter.ToUInt32(bytes, i * 4); break;
                    default:   throw new Exception($"unsupported index componentType {componentType}");
                }
            }
            return result;
        }

        static byte[] SliceAccessor(List<byte[]> buffers,
                                    List<object> bufferViews,
                                    Dictionary<string, object> acc,
                                    int expectedBytes)
        {
            int bvIdx     = (int)(long)acc["bufferView"];
            int accOffset = acc.ContainsKey("byteOffset") ? (int)(long)acc["byteOffset"] : 0;
            var bv        = (Dictionary<string, object>)bufferViews[bvIdx];
            int bufIdx    = (int)(long)bv["buffer"];
            int bvOffset  = bv.ContainsKey("byteOffset") ? (int)(long)bv["byteOffset"] : 0;
            int startByte = bvOffset + accOffset;
            var slice = new byte[expectedBytes];
            Buffer.BlockCopy(buffers[bufIdx], startByte, slice, 0, expectedBytes);
            return slice;
        }

        static List<object> AsList(Dictionary<string, object> doc, string key)
        {
            if (!doc.ContainsKey(key)) return new List<object>();
            return (List<object>)doc[key];
        }
    }

    // ── Minimal JSON parser ───────────────────────────────────────────
    // Unity's JsonUtility doesn't handle arbitrary maps — and pulling
    // Newtonsoft into a tiny demo project is overkill. This is a tiny
    // recursive-descent JSON parser that handles exactly what glTF
    // files produce: nested objects, arrays, strings, numbers (long or
    // double depending on whether there's a decimal), booleans, null.
    static class MiniJson
    {
        public static object Deserialize(string json)
        {
            int pos = 0;
            object o = ParseValue(json, ref pos);
            return o;
        }

        static object ParseValue(string s, ref int p)
        {
            SkipWhitespace(s, ref p);
            char c = s[p];
            if (c == '{') return ParseObject(s, ref p);
            if (c == '[') return ParseArray(s, ref p);
            if (c == '"') return ParseString(s, ref p);
            if (c == 't' || c == 'f') return ParseBool(s, ref p);
            if (c == 'n') { p += 4; return null; }
            return ParseNumber(s, ref p);
        }

        static Dictionary<string, object> ParseObject(string s, ref int p)
        {
            var d = new Dictionary<string, object>();
            p++;  // consume '{'
            SkipWhitespace(s, ref p);
            if (s[p] == '}') { p++; return d; }
            while (true)
            {
                SkipWhitespace(s, ref p);
                string key = ParseString(s, ref p);
                SkipWhitespace(s, ref p);
                p++;  // consume ':'
                object value = ParseValue(s, ref p);
                d[key] = value;
                SkipWhitespace(s, ref p);
                if (s[p] == ',') { p++; continue; }
                if (s[p] == '}') { p++; return d; }
            }
        }

        static List<object> ParseArray(string s, ref int p)
        {
            var list = new List<object>();
            p++;  // consume '['
            SkipWhitespace(s, ref p);
            if (s[p] == ']') { p++; return list; }
            while (true)
            {
                list.Add(ParseValue(s, ref p));
                SkipWhitespace(s, ref p);
                if (s[p] == ',') { p++; continue; }
                if (s[p] == ']') { p++; return list; }
            }
        }

        static string ParseString(string s, ref int p)
        {
            p++;  // consume opening '"'
            var sb = new StringBuilder();
            while (p < s.Length && s[p] != '"')
            {
                if (s[p] == '\\' && p + 1 < s.Length)
                {
                    char esc = s[p + 1];
                    if (esc == '"' || esc == '\\' || esc == '/') sb.Append(esc);
                    else if (esc == 'n') sb.Append('\n');
                    else if (esc == 't') sb.Append('\t');
                    else if (esc == 'r') sb.Append('\r');
                    else sb.Append(esc);
                    p += 2;
                }
                else { sb.Append(s[p]); p++; }
            }
            p++;  // consume closing '"'
            return sb.ToString();
        }

        static object ParseNumber(string s, ref int p)
        {
            int start = p;
            while (p < s.Length && "0123456789+-.eE".IndexOf(s[p]) >= 0) p++;
            string n = s.Substring(start, p - start);
            // Distinguish long vs double — every int-valued field in glTF
            // (counts, indices, byteOffsets) needs to fit a long; vector
            // values are doubles.
            if (n.IndexOfAny(new[] { '.', 'e', 'E' }) >= 0)
                return double.Parse(n, System.Globalization.CultureInfo.InvariantCulture);
            return long.Parse(n, System.Globalization.CultureInfo.InvariantCulture);
        }

        static object ParseBool(string s, ref int p)
        {
            if (s[p] == 't') { p += 4; return true; }
            p += 5; return false;
        }

        static void SkipWhitespace(string s, ref int p)
        {
            while (p < s.Length && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
        }
    }
}
#endif
