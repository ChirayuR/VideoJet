# One component. Consumers see include/videojet.h only; the vendored decoder and our sources stay private.
# CONFIGURE_DEPENDS re-globs on the next build, but IDF's requirements scan runs this file in script mode,
# where it is an error, so it is guarded.
if(CMAKE_BUILD_EARLY_EXPANSION)
    file(GLOB dec_srcs "h264bsd/*.c")
else()
    file(GLOB dec_srcs CONFIGURE_DEPENDS "h264bsd/*.c")
endif()
set(vj_srcs "src/videojet.c" "src/vj_au.c" "src/vj_alloc.c" "src/vj_worker.c")
if(CONFIG_VJ_PIE)
    list(APPEND vj_srcs "src/vj_mc_pie.S" "src/vj_wr_pie.S" "src/vj_idct_pie.S")
endif()
idf_component_register(SRCS ${vj_srcs} ${dec_srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src" "h264bsd"
                       PRIV_REQUIRES heap
                       LDFRAGMENTS "linker.lf")
# IDF's -Werror=all beats a bare -Wno-error, so this is a point fix for a known GCC false positive in
# h264bsd_pic_order_cnt.c (both variables are set and read under the same absFrameNum > 0 test). Other tags may
# need the same as optimisation flags change. It is set on the vendored sources only: our own code stays strict.
# (IDF's requirements scan returns from idf_component_register, so nothing below runs in script mode.)
set_source_files_properties(${dec_srcs} PROPERTIES COMPILE_OPTIONS -Wno-error=maybe-uninitialized)
if(CONFIG_VJ_ERROR_PRINT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE _ERROR_PRINT)
endif()
if(CONFIG_VJ_DEBLOCK_WORKER)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VJ_DEBLOCK_WORKER)
endif()
