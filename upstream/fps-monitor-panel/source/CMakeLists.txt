cmake_minimum_required (VERSION 3.20)

# fps-monitor-panel: standalone FPS monitor payload.
# Produces exactly one deliverable, ${CMAKE_BINARY_DIR}/bin/fps-monitor.elf
# (with monitor_shellui.elf embedded).
project("fps-monitor-panel")

set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(OnionTarget)
get_filename_component(FPSMON_ROOT "${PROJECT_ROOT}/.." ABSOLUTE)
set(FPSMON_THIRD_PARTY_DIR "${FPSMON_ROOT}/third_party" CACHE PATH
	"Third-party dependency source directory")
set(FPSMON_PS5_STUB_DIR "${PROJECT_ROOT}/platform/ps5/stubs")

# ELFs to bin/, static libs to lib/ under the build directory.
set(FPSMON_OUT_BIN "${CMAKE_BINARY_DIR}/bin" CACHE PATH "ELF output directory")
set(FPSMON_OUT_LIB "${CMAKE_BINARY_DIR}/lib" CACHE PATH "Built static-lib output directory")
file(MAKE_DIRECTORY "${FPSMON_OUT_BIN}" "${FPSMON_OUT_LIB}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${FPSMON_OUT_BIN})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${FPSMON_OUT_LIB})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${FPSMON_OUT_LIB})

# First-party shared headers.
include_directories("${PROJECT_ROOT}/include")
include_directories(SYSTEM "${PS5_PAYLOAD_SDK}")
include_directories(SYSTEM "${PS5_PAYLOAD_SDK}/include")
link_directories("${FPSMON_OUT_LIB}" "${FPSMON_PS5_STUB_DIR}")

add_subdirectory(libhijacker)
# Shared proc/ucred before NineS (inject path links these symbols).
add_subdirectory(libonion_proc)
add_subdirectory(libonion_fps)
add_subdirectory(libonion_platform)
add_subdirectory(libonion_ipc)
add_subdirectory(libonion_settings)
add_subdirectory(libonion_detour)
add_subdirectory(libonion_ready)
add_subdirectory(libonion_elfldr)
add_subdirectory(libNineS)
add_subdirectory(fps_monitor)
