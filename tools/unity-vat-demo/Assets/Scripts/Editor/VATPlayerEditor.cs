// VATPlayerEditor — custom Inspector for VATPlayer that adds an
// "Auto-Wire from Bake" button. The button replaces the need to fight
// Unity's Mesh picker, which by default hides sub-assets nested inside
// a Model importer (.fbx / .gltf). The actual Mesh asset that drives
// the VAT lives as a sub-asset of `source.fbx`, not as a top-level
// Mesh — that's why the picker shows it as empty even though the file
// is in the project.
//
// Why a custom Editor and not just rely on BootstrapVAT's auto-wirer:
// the `ObjectFactory.componentWasAdded` hook only fires the *moment*
// the component is added. If Unity is still importing the FBX at that
// moment (common on first project open), the AssetDatabase lookup
// returns null and the field stays empty. A button puts the wiring
// under the user's control and runs whenever they click it — after
// the import has finished, after a re-bake, etc.

#if UNITY_EDITOR
using System.IO;
using UnityEditor;
using UnityEngine;

namespace QtMeshEditor.VAT.Editor
{
    [CustomEditor(typeof(VATPlayer))]
    public class VATPlayerEditor : UnityEditor.Editor
    {
        public override void OnInspectorGUI()
        {
            DrawDefaultInspector();

            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Bake helpers", EditorStyles.boldLabel);

            using (new EditorGUILayout.HorizontalScope())
            {
                if (GUILayout.Button("Auto-Wire from Bake", GUILayout.Height(28)))
                {
                    foreach (var t in targets)
                    {
                        if (t is VATPlayer p) BootstrapVAT.AutoWire(p, verbose: true);
                    }
                }
            }
            EditorGUILayout.HelpBox(
                "Click \"Auto-Wire from Bake\" to (re)populate Source Mesh, " +
                "Position Texture, Diffuse Texture, Frame Count, and Bounds " +
                "from the bake under Assets/VAT/Rumba. The mesh lives as a " +
                "sub-asset of source.fbx — Unity's picker hides those by " +
                "default, so use this button instead of the field's ⊙ icon.",
                MessageType.Info);
        }
    }
}
#endif
