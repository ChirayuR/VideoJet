cmake_minimum_required(VERSION 3.16)
# On-board oracle for the PIE 4x4 inverse transform: see README.md. It builds the library like any consumer.
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../components)
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vj_idct_pie_test)
