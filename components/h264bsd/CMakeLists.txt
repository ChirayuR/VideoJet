file(GLOB srcs "src/*.c")
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "src")
# Vendored code: keep its warnings visible but never fatal. IDF compiles with -Werror=all, which a
# bare -Wno-error does not override, so downgrade the one warning GCC 14 raises here: a false
# positive in h264bsd_pic_order_cnt.c (both variables are set and read under the same
# `absFrameNum > 0` test).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=maybe-uninitialized -D_ERROR_PRINT)
