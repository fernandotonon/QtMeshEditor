/* MinGW SAL shim for the MSVC-built ONNX Runtime headers (force-included via
 * cmake/OnnxRuntime.cmake, MinGW only).
 *
 * onnxruntime_c_api.h annotates its API with MSVC SAL macros. MinGW-w64's
 * sal.h covers the classic set but lags the newer annotations (the 13.1
 * toolchain lacks _Frees_ptr_opt_, which ORT 1.20 uses) — every gap is a
 * hard compile error. Include the real sal.h first, then no-op define ONLY
 * whatever it did not provide, covering the full annotation inventory the
 * ORT header actually uses (grep _[A-Z][a-z] over onnxruntime_c_api.h). */
#ifndef QTMESH_ONNX_MINGW_SAL_SHIM_H
#define QTMESH_ONNX_MINGW_SAL_SHIM_H

#if defined(__MINGW32__) || defined(__MINGW64__)
#include <sal.h>

#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif
#ifndef _Check_return_
#define _Check_return_
#endif
#ifndef _In_
#define _In_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _In_opt_z_
#define _In_opt_z_
#endif
#ifndef _In_z_
#define _In_z_
#endif
#ifndef _In_reads_
#define _In_reads_(s)
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _Inout_opt_
#define _Inout_opt_
#endif
#ifndef _Inout_updates_
#define _Inout_updates_(s)
#endif
#ifndef _Inout_updates_all_
#define _Inout_updates_all_(s)
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Out_opt_
#define _Out_opt_
#endif
#ifndef _Out_writes_
#define _Out_writes_(s)
#endif
#ifndef _Out_writes_all_
#define _Out_writes_all_(s)
#endif
#ifndef _Out_writes_bytes_all_
#define _Out_writes_bytes_all_(s)
#endif
#ifndef _Outptr_
#define _Outptr_
#endif
#ifndef _Outptr_result_buffer_maybenull_
#define _Outptr_result_buffer_maybenull_(s)
#endif
#ifndef _Outptr_result_maybenull_
#define _Outptr_result_maybenull_
#endif
#ifndef _Ret_maybenull_
#define _Ret_maybenull_
#endif
#ifndef _Ret_notnull_
#define _Ret_notnull_
#endif
#ifndef _Return_type_success_
#define _Return_type_success_(e)
#endif
#ifndef _Success_
#define _Success_(e)
#endif

#endif /* __MINGW32__ || __MINGW64__ */
#endif /* QTMESH_ONNX_MINGW_SAL_SHIM_H */
