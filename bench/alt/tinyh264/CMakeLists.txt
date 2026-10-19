# tinyH264 (github.com/udevbe/tinyh264 at 524a702, Apache-2.0), fetched unmodified into native/ by tools/fetch_alt.sh.
file(GLOB srcs "native/*.c")
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "native")
# The same point fix as VideoJet's vendored h264bsd (components/videojet/CMakeLists.txt): IDF's -Werror=all.
set_source_files_properties(${srcs} PROPERTIES COMPILE_OPTIONS -Wno-error=maybe-uninitialized)
