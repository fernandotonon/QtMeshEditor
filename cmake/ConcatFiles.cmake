# Build-time file concatenation (cmake -P): add_custom_command has no shell,
# so `-E cat ... > out` cannot redirect — this script does it portably.
#   cmake -DOUT=<path> "-DIN=<f1>|<f2>|..." -P ConcatFiles.cmake
# The separator is '|', NOT ';' — an escaped semicolon list survives POSIX
# shells (which strip the backslash) but reaches CMake as one giant literal
# "a\;b\;c" path under Windows cmd, so every EXISTS failed and the 3.38.0
# Windows zip shipped a 0-byte trellis-cli.THIRD_PARTY_LICENSES.txt.
if(NOT OUT OR NOT IN)
    message(FATAL_ERROR "ConcatFiles.cmake: OUT and IN are required")
endif()
string(REPLACE "|" ";" _in_list "${IN}")
set(_acc "")
set(_missing "")
foreach(_f IN LISTS _in_list)
    if(EXISTS "${_f}")
        file(READ "${_f}" _c)
        string(APPEND _acc "${_c}\n----------------------------------------\n")
    else()
        list(APPEND _missing "${_f}")
    endif()
endforeach()
if(_acc STREQUAL "")
    message(FATAL_ERROR "ConcatFiles.cmake: NO input files existed — would "
                        "write an empty ${OUT}. Missing: ${_missing}")
endif()
if(_missing)
    message(WARNING "ConcatFiles.cmake: skipped missing inputs: ${_missing}")
endif()
file(WRITE "${OUT}" "${_acc}")
