# CONFIGURE_DEPENDS: a new src/*.c re-runs CMake on the next incremental build (the host Makefile's wildcard sees it at
# once). IDF's requirements scan runs this file in script mode, where CONFIGURE_DEPENDS is an error: plain glob there.
if(CMAKE_BUILD_EARLY_EXPANSION)
    file(GLOB srcs "src/*.c")
else()
    file(GLOB srcs CONFIGURE_DEPENDS "src/*.c")
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "src")
# Vendored code: its warnings stay visible. IDF compiles with -Werror=all, which a bare -Wno-error does
# not override, so this is a point fix for one known GCC false positive (h264bsd_pic_order_cnt.c: both
# variables are set and read under the same `absFrameNum > 0` test). Other warning tags may need the
# same treatment as the optimisation flags change.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=maybe-uninitialized -D_ERROR_PRINT)
if(VJ_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC VJ_PROFILE=1)
endif()
