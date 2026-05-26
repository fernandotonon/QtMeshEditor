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
                    mats.Add(m);
                }
                r.materials = ms;
            }
            _mats = mats.ToArray();
        }

        void Update()
        {
            if (_mats == null || _mats.Length == 0) return;
            float safeFrames = Mathf.Max(1, frames);
            _currentFrame += Time.deltaTime * fps;
            _currentFrame %= safeFrames;
            foreach (var m in _mats)
                m.SetFloat("_frame", _currentFrame);
        }
    }
}
