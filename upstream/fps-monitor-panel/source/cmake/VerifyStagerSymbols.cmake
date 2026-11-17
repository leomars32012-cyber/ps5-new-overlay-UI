# Validate the stager's linked symbol boundaries in libNineS.a.
# The stager, breakpoint label, and end label must all be present and ordered.

if(NOT DEFINED NM OR NM STREQUAL "")
  message(FATAL_ERROR "CMAKE_NM is unavailable; cannot validate stager symbols")
endif()
if(NOT DEFINED ARCHIVE OR NOT EXISTS "${ARCHIVE}")
  message(FATAL_ERROR "NineS archive is missing: ${ARCHIVE}")
endif()

execute_process(
  COMMAND "${NM}" -n "${ARCHIVE}"
  RESULT_VARIABLE nm_result
  OUTPUT_VARIABLE nm_output
  ERROR_VARIABLE nm_error
)
if(NOT nm_result EQUAL 0)
  message(FATAL_ERROR "Unable to inspect NineS stager symbols: ${nm_error}")
endif()

string(REPLACE "\r\n" "\n" nm_output "${nm_output}")
string(REPLACE "\n" ";" nm_lines "${nm_output}")

set(stager_addr "")
set(breakpoint_addr "")
set(stager_end_addr "")

foreach(line IN LISTS nm_lines)
  string(STRIP "${line}" line)
  if(line MATCHES "^([0-9A-Fa-f]+)[ \t]+[A-Za-z][ \t]+stager$")
    set(stager_addr "${CMAKE_MATCH_1}")
  elseif(line MATCHES "^([0-9A-Fa-f]+)[ \t]+[A-Za-z][ \t]+stager_breakpoint$")
    set(breakpoint_addr "${CMAKE_MATCH_1}")
  elseif(line MATCHES "^([0-9A-Fa-f]+)[ \t]+[A-Za-z][ \t]+stager_end$")
    set(stager_end_addr "${CMAKE_MATCH_1}")
  endif()
endforeach()

if(stager_addr STREQUAL "" OR breakpoint_addr STREQUAL "" OR
   stager_end_addr STREQUAL "")
  message(FATAL_ERROR
    "Missing stager boundary symbol(s) in ${ARCHIVE}: "
    "stager=${stager_addr}, breakpoint=${breakpoint_addr}, "
    "stager_end=${stager_end_addr}")
endif()

math(EXPR stager_start_value "0x${stager_addr}")
math(EXPR breakpoint_value "0x${breakpoint_addr}")
math(EXPR stager_end_value "0x${stager_end_addr}")
math(EXPR stager_size "${stager_end_value} - ${stager_start_value}")

if(stager_size LESS_EQUAL 0 OR stager_size GREATER 4096 OR
   breakpoint_value LESS_EQUAL stager_start_value OR
   breakpoint_value GREATER_EQUAL stager_end_value)
  message(FATAL_ERROR
    "Invalid stager layout: start=0x${stager_addr}, "
    "breakpoint=0x${breakpoint_addr}, end=0x${stager_end_addr}, "
    "size=${stager_size}")
endif()

message(STATUS
  "Validated stager layout: start=0x${stager_addr}, "
  "breakpoint=0x${breakpoint_addr}, end=0x${stager_end_addr}, "
  "size=${stager_size} bytes")
