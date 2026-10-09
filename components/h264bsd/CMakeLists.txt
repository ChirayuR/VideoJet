file(GLOB srcs "src/*.c")
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "src")
# Vendored code: its warnings stay visible. IDF compiles with -Werror=all, which a bare -Wno-error does
# not override, so this is a point fix for one known GCC false positive (h264bsd_pic_order_cnt.c: both
# variables are set and read under the same `absFrameNum > 0` test). Other warning tags may need the
# same treatment as the optimisation flags change.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=maybe-uninitialized -D_ERROR_PRINT)
