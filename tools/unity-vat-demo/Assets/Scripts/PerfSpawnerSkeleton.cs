// PerfSpawnerSkeleton — apples-to-apples comparison against
// PerfSpawnerVAT. Spawns N copies of the imported glTF prefab so Unity's
// own SkinnedMeshRenderer + Animator handle the skinning. The FPS
// difference between the two scenes is the cost of per-instance bone
// matrix upload + skinning vs. one texture sample per vertex.
//
// To keep the comparison honest:
//   • Same grid size and spacing as PerfSpawnerVAT.
//   • Same fps target.
//   • Phase-offset each instance's animation playback so the GPU can't
//     coalesce identical poses across draws (matches the VAT spawner).
//   • Shadows OFF on both scenes (configure on the prefab).

using UnityEngine;

namespace QtMeshEditor.VAT
{
    public class PerfSpawnerSkeleton : MonoBehaviour
    {
        [Tooltip("Drag the imported source.gltf prefab here. Must have an Animator and a SkinnedMeshRenderer.")]
        public GameObject sourcePrefab;

        public int   gridSize    = 32;
        public float spacing     = 1.6f;
        public float phaseJitter = 1.5f;     // seconds — desyncs identical work

        Animator[] _animators;

        void Start()
        {
            if (sourcePrefab == null)
            {
                Debug.LogError("PerfSpawnerSkeleton: drag source.gltf into sourcePrefab.", this);
                enabled = false;
                return;
            }

            int total = gridSize * gridSize;
            _animators = new Animator[total];
            float half = (gridSize - 1) * spacing * 0.5f;

            for (int i = 0; i < total; i++)
            {
                int gx = i % gridSize;
                int gz = i / gridSize;

                var go = Instantiate(sourcePrefab, transform);
                go.name = $"Sk_{i}";
                go.transform.localPosition = new Vector3(gx * spacing - half, 0f, gz * spacing - half);

                // Per-instance animation phase: nudge the Animator into a
                // unique state offset using a deterministic per-index seed.
                var anim = go.GetComponentInChildren<Animator>();
                if (anim != null && anim.runtimeAnimatorController != null)
                {
                    float phase = (i * 7919 % 1000) * (phaseJitter / 1000f);
                    // Play the default state at a custom normalized time
                    // so frame 0 isn't visually identical across all
                    // 1000 dancers.
                    anim.Play(0, 0, phase);
                }
                _animators[i] = anim;
            }
        }
    }
}
