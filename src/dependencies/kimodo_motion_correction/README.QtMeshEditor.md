# NVIDIA Kimodo MotionCorrection (vendored)

Source: https://github.com/nv-tlabs/kimodo, revision `e1c219c99b4b94ba48be1fb6a1cbc64ecee4c954`,
directory `MotionCorrection/src/cpp` (the `Math/` and `AnimProcessing/` libraries plus `Compiler.h`,
`Debug.h`, `Platform.h`). Licence: Apache-2.0 (`LICENSE-Apache-2.0.txt`, copied from the repository root).
`BindingsPython.cpp` is not vendored: `src/AIMotion/KimodoCorrection.cpp` does the same setup natively.

`Math/sse2neon.h` is sse2neon v1.8.0 (MIT, licence text in the file header,
https://github.com/DLTcollab/sse2neon), SHA-256 `07723c9f9457dd4316f1fde3dd4eb6f31dd67d9955f6c21f4e609ac1698be48a`.

Local modifications (all marked in the sources):

1. `Math/SIMD.h`: include `sse2neon.h` on ARM instead of `<immintrin.h>`, fenced with `push_macro`/`pop_macro`
   because sse2neon defines its own `FORCE_INLINE`.
2. `Compiler.h`: `FORCE_INLINE` is defined for Clang too (upstream defines it for MSVC and GCC only).
3. `Math/Vector.inl` `Vector::Swizzle`: `_mm_permutevar_ps` (AVX) is replaced by a portable lane gather unless
   the build targets AVX, so the library needs SSE4.1 at most on x86 and runs on CPUs without AVX.
4. `Math/Scalar.h`, `Math/Vector.h`: `Compiler.h` / `Debug.h` are included by relative path (`../`), because the
   editor's include path made a different `Debug.h` win and the build lost the `ASSERT` macros.

Built by `cmake/KimodoMotionCorrection.cmake`.
