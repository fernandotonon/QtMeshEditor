# Build-time file concatenation (cmake -P): add_custom_command has no shell,
# so `-E cat ... > out` cannot redirect — this script does it portably.
#   cmake -DOUT=<path> -DIN=<f1>;<f2>;... -P ConcatFiles.cmake
if(NOT OUT OR NOT IN)
    message(FATAL_ERROR "ConcatFiles.cmake: OUT and IN are required")
endif()
set(_acc "")
foreach(_f IN LISTS IN)
    if(EXISTS "${_f}")
        file(READ "${_f}" _c)
        string(APPEND _acc "${_c}\n----------------------------------------\n")
    endif()
endforeach()
file(WRITE "${OUT}" "${_acc}")
