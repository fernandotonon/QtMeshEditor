// OrbitCamera — mouse-drag orbit + wheel zoom around a fixed target.
// Mirrors the Godot demo's OrbitCamera.gd so the two demos behave the
// same.

using UnityEngine;

namespace QtMeshEditor.VAT
{
    [RequireComponent(typeof(Camera))]
    public class OrbitCamera : MonoBehaviour
    {
        public Vector3 target  = new Vector3(0f, 1f, 0f);
        public float   distance = 4.5f;
        public float   yawDeg   = 15f;
        public float   pitchDeg = -10f;

        public float orbitSpeed = 0.35f;
        public float zoomSpeed  = 1.0f;
        public float minDistance = 1.5f;
        public float maxDistance = 12f;
        public float minPitch    = -85f;
        public float maxPitch    =  85f;

        bool _dragging;

        void Update()
        {
            // Mouse drag — left or middle button.
            if (Input.GetMouseButtonDown(0) || Input.GetMouseButtonDown(2)) _dragging = true;
            if (Input.GetMouseButtonUp(0)   || Input.GetMouseButtonUp(2))   _dragging = false;

            if (_dragging)
            {
                yawDeg   += Input.GetAxis("Mouse X") * orbitSpeed * 30f;
                pitchDeg -= Input.GetAxis("Mouse Y") * orbitSpeed * 30f;
                pitchDeg = Mathf.Clamp(pitchDeg, minPitch, maxPitch);
            }

            // Wheel zoom (`Mouse ScrollWheel` returns lines, not pixels).
            float wheel = Input.GetAxis("Mouse ScrollWheel");
            if (wheel != 0f)
            {
                distance *= 1f - wheel * zoomSpeed;
                distance = Mathf.Clamp(distance, minDistance, maxDistance);
            }
            // Keyboard +/- fallback (matches the Godot demo).
            if (Input.GetKey(KeyCode.Equals) || Input.GetKey(KeyCode.Plus) || Input.GetKey(KeyCode.KeypadPlus))
                distance = Mathf.Max(minDistance, distance - zoomSpeed * Time.deltaTime * 3f);
            if (Input.GetKey(KeyCode.Minus)  || Input.GetKey(KeyCode.KeypadMinus))
                distance = Mathf.Min(maxDistance, distance + zoomSpeed * Time.deltaTime * 3f);

            // Compose the transform once per frame from the polar state.
            var rot = Quaternion.Euler(pitchDeg, yawDeg, 0f);
            var off = rot * new Vector3(0f, 0f, -distance);
            transform.position = target + off;
            transform.LookAt(target);
        }
    }
}
