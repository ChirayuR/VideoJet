cmake_minimum_required(VERSION 3.16)
# The bench is a consumer of the library components, exactly like a real app would be.
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(videojet_bench)
