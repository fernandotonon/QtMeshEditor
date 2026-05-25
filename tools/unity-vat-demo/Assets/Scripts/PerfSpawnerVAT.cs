// PerfSpawnerVAT — spawns N VAT-driven dancers in a grid for the perf
// scene. Each instance is a child GameObject with VATPlayer + MeshFilter
// + MeshRenderer. A shared clock advances every frame; we push it into
// every player via SetCurrentFrame() so there are no per-instance host-
// side material updates beyond one shader-parameter write per dancer.
//
// Compared to PerfSpawnerSkeleton, this is the apples-to-apples perf
// comparison: same number of instances, same mesh, same shadow setup —
// only the animation path differs (texture sample vs bone matrix
// upload).

using UnityEngine;

namespace QtMeshEditor.VAT
{
    public class PerfSpawnerVAT : MonoBehaviour
    {
        [Header("Bake")]
        public Mesh      sourceMesh;
        public Texture2D positionTexture;
        public Texture2D diffuseTexture;
        public int       frameCount = 71;
        public Vector3   boundsMin  = new Vector3(-0.7f, -0.1f, -0.6f);
        public Vector3   boundsMax  = new Vector3( 0.6f,  2.0f,  0.4f);

        [Header("Grid")]
        public int   gridSize    = 32;       // 32×32 = 1024 instances
        public float spacing     = 1.6f;
        public float phaseJitter = 30f;      // frames; desyncs identical work

        [Header("Playback")]
        public float fps = 30f;

        VATPlayer[] _players;
        float       _clock;

        void Start()
        {
            if (sourceMesh == null || positionTexture == null)
            {
                Debug.LogError("PerfSpawnerVAT: assign sourceMesh + positionTexture in the inspector.", this);
                enabled = false;
                return;
            }

            int total = gridSize * gridSize;
            _players = new VATPlayer[total];

            // Center the grid on the origin so the camera doesn't need a
            // per-spawn-count offset.
            float half = (gridSize - 1) * spacing * 0.5f;

            for (int i = 0; i < total; i++)
            {
                int gx = i % gridSize;
                int gz = i / gridSize;

                var go = new GameObject($"VAT_{i}");
                go.transform.SetParent(transform, false);
                go.transform.localPosition = new Vector3(gx * spacing - half, 0f, gz * spacing - half);

                go.AddComponent<MeshFilter>();
                go.AddComponent<MeshRenderer>();
                var p = go.AddComponent<VATPlayer>();
                p.sourceMesh      = sourceMesh;
                p.positionTexture = positionTexture;
                p.diffuseTexture  = diffuseTexture;
                p.frameCount      = frameCount;
                p.boundsMin       = boundsMin;
                p.boundsMax       = boundsMax;
                p.fps             = fps;
                // Critical: kill self-driven advance — the spawner pushes
                // a single shared clock to every instance with an extra
                // per-instance phase offset so the GPU can't coalesce
                // identical work across draws.
                p.selfDriven = false;
                _players[i] = p;
            }
            // Force-initialise so Start() runs in the same frame.
            // (Unity defers child Start()s by default; calling
            // SetCurrentFrame before Start would no-op because _material
            // is still null.)
        }

        void Update()
        {
            _clock += Time.deltaTime * fps;
            if (_players == null) return;
            for (int i = 0; i < _players.Length; i++)
            {
                if (_players[i] == null) continue;
                // Deterministic per-instance phase — same formula as the
                // Godot demo so the visual feel matches.
                float phase = (i * 7919) % phaseJitter;  // 7919 is a small prime → spreads phases
                _players[i].SetCurrentFrame(_clock + phase);
            }
        }
    }
}
