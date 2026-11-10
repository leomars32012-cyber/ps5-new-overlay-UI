cmake_minimum_required(VERSION 3.20)

# This repository is built on the vendored fps-monitor-panel project.
# The legacy src/ and include/ tree is retained only as a donor/reference
# for telemetry that the upstream foundation does not provide.
project(ps5_overlay_workspace LANGUAGES C CXX)

if(NOT DEFINED PS5_PAYLOAD_SDK)
    if(DEFINED ENV{PS5_PAYLOAD_SDK})
        set(PS5_PAYLOAD_SDK "$ENV{PS5_PAYLOAD_SDK}")
    elseif(DEFINED ENV{PS5SDK})
        set(PS5_PAYLOAD_SDK "$ENV{PS5SDK}")
    else()
        message(FATAL_ERROR
            "PS5_PAYLOAD_SDK is required. For host-only layout tests run "
            "upstream/fps-monitor-panel/scripts/test.sh.")
    endif()
endif()

if(NOT EXISTS "${PS5_PAYLOAD_SDK}/bin/prospero-cmake")
    message(FATAL_ERROR
        "Invalid PS5_PAYLOAD_SDK: ${PS5_PAYLOAD_SDK} "
        "(bin/prospero-cmake was not found).")
endif()

add_subdirectory(upstream/fps-monitor-panel/source)
