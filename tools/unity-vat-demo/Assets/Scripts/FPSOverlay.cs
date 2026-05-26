// FPSOverlay — minimal IMGUI overlay showing avg/min FPS over a rolling
// window. The "min over the last second" metric is the honest one — avg
// hides hitches that show up to the eye.

using System.Collections.Generic;
using UnityEngine;

namespace QtMeshEditor.VAT
{
    public class FPSOverlay : MonoBehaviour
    {
        public int   windowFrames = 60;   // ~1 second at 60 fps
        public Color textColor    = Color.white;
        public int   fontSize     = 20;

        readonly Queue<float> _samples = new Queue<float>();
        float _smoothedAvg;
        float _windowMin = float.MaxValue;

        void OnValidate()
        {
            // Negative / zero from the Inspector would underflow the
            // dequeue loop below; clamp on edit so the inspector value
            // is always safe.
            windowFrames = Mathf.Max(1, windowFrames);
        }

        void Update()
        {
            float dt = Time.unscaledDeltaTime;
            float fps = dt > 0 ? 1f / dt : 0f;

            _samples.Enqueue(fps);
            int frames = Mathf.Max(1, windowFrames);
            while (_samples.Count > frames) _samples.Dequeue();

            float sum = 0f, mn = float.MaxValue;
            foreach (var s in _samples) { sum += s; if (s < mn) mn = s; }
            _smoothedAvg = sum / Mathf.Max(1, _samples.Count);
            _windowMin   = mn;
        }

        void OnGUI()
        {
            var style = new GUIStyle(GUI.skin.label)
            {
                fontSize  = fontSize,
                fontStyle = FontStyle.Bold,
                normal    = { textColor = textColor },
            };
            // Drop-shadow for legibility against a busy scene.
            var shadow = new GUIStyle(style) { normal = { textColor = Color.black } };

            string text = string.Format(
                "FPS  avg {0:0.0}   min (1s) {1:0.0}",
                _smoothedAvg, _windowMin == float.MaxValue ? 0f : _windowMin);

            GUI.Label(new Rect(20 + 2, 14 + 2, 600, 60), text, shadow);
            GUI.Label(new Rect(20,     14,     600, 60), text, style);
        }
    }
}
