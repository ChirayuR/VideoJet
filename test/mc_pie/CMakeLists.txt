cmake_minimum_required(VERSION 3.16)
# On-board oracle for the PIE motion-compensation kernels: see README.md. It builds the library like any consumer.
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../components)
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vj_mc_pie_test)
