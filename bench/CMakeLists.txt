cmake_minimum_required(VERSION 3.16)
# The bench is a consumer of the library components, exactly like a real app would be.
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../components)
# Comparison builds: VJ_BENCH_DEC=esp_h264 or tinyh264 in the environment decodes with that instead (tools/fetch_alt.sh).
# VideoJet stays out of the build: its private h264bsd and theirs define the same symbols.
if(DEFINED ENV{VJ_BENCH_DEC})
    set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../components/vj_hash ${CMAKE_CURRENT_LIST_DIR}/alt/$ENV{VJ_BENCH_DEC})
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(videojet_bench)
