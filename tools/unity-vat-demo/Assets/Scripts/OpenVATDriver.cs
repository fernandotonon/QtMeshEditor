// OpenVATDriver — drives `_frame` on a Renderer's OpenVAT material
// every Update, advancing through the bake at the configured fps.
// Use this when the shader's `_UseTime` self-tick is unreliable
// (Unity 6 + URP under some setups appears to leave `_Time.y` at 0
// in the player). The driver overrides _UseTime to false on every
// instance and writes `_frame` directly so we're never at the mercy
// of the shader's internal time wiring.

using UnityEngine;

namespace QtMeshEditor.VAT
{
    [DisallowMultipleComponent]
    public class OpenVATDriver : MonoBehaviour
    {
        [Tooltip("Frames per second to advance the bake at.")]
        public float fps = 30f;
        [Tooltip("Total frames in the bake. Matches `os-remap.Frames` from the sidecar.")]
        public int frames = 71;

        Material[] _mats;
        float _currentFrame;

        void Start()
        {
            Debug.Log($"OpenVATDriver.Start on {gameObject.name}");
            // Collect every Material on every Renderer in this hierarchy
            // and force them to manual-time mode. Using `Renderer.materials`
            // (plural, returns instance copies) so each instance has its
            // own _frame and we don't fight Unity's shared-material batching.
            var renderers = GetComponentsInChildren<Renderer>(includeInactive: true);
            var mats = new System.Collections.Generic.List<Material>();
            foreach (var r in renderers)
            {
                var ms = r.materials;
                foreach (var m in ms)
                {
                    if (m == null) continue;
                    // Only touch materials that look like OpenVAT (have _frame).
                    if (!m.HasProperty("_frame")) continue;
                    if (m.HasProperty("_UseTime")) m.SetFloat("_UseTime", 0f);
                    if (m.HasProperty("_UseTIme")) m.SetFloat("_UseTIme", 0f);  // alt spelling
                    // Also disable the keyword in case ShaderGraph
                    // compiled a static-branch variant rather than a
                    // uniform-branch — the editor pipeline enabled
                    // _USETIME_ON, which we must undo here so the
                    // shader runs the `frame` branch instead.
                    m.DisableKeyword("_USETIME_ON");
                    // Also ensure exaggeration is non-zero — the
                    // ShaderGraph default is 0 (which silently mutes
                    // deformation), and even the editor's fixup might
                    // get clobbered by ShaderGraph variant cooking.
                    if (m.HasProperty("_exaggeration") && m.GetFloat("_exaggeration") < 0.01f)
                    {
                        m.SetFloat("_exaggeration", 1f);
                        Debug.Log("OpenVATDriver: restored _exaggeration=1 on " + m.name);
                    }
                    mats.Add(m);
                }
                r.materials = ms;
            }
            _mats = mats.ToArray();
            Debug.Log($"OpenVATDriver: bound {_mats.Length} VAT materials, fps={fps}, frames={frames}");
        }

        void Update()
        {
            if (_mats == null || _mats.Length == 0) return;
            float safeFrames = Mathf.Max(1, frames);
            _currentFrame += Time.deltaTime * fps;
            _currentFrame %= safeFrames;
            // Log first 5 frames so we can confirm the driver IS pushing values.
            if (Time.frameCount < 5)
                Debug.Log($"OpenVATDriver: frame {Time.frameCount} → _frame={_currentFrame:F2} / {safeFrames}");
            foreach (var m in _mats)
                m.SetFloat("_frame", _currentFrame);
        }
    }
}
