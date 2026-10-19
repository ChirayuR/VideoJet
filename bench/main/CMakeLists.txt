if(DEFINED ENV{VJ_BENCH_DEC})
    # Comparison build (bench/CMakeLists.txt): alt_<dec>.c puts that decoder behind VideoJet's API and VideoJet's AU
    # splitter is compiled in directly, so bench_main.c times and hashes every decoder with the same code.
    idf_component_register(SRCS "bench_main.c" "alt_$ENV{VJ_BENCH_DEC}.c" "../../components/videojet/src/vj_au.c"
                           INCLUDE_DIRS "." "../../components/videojet/include"
                           PRIV_REQUIRES $ENV{VJ_BENCH_DEC} vj_hash esp_partition esp_timer spi_flash)
else()
    idf_component_register(SRCS "bench_main.c" INCLUDE_DIRS "."
                           PRIV_REQUIRES videojet vj_hash esp_partition esp_timer spi_flash)
endif()
# The hold-on score: idf.py -DVJ_BENCH_HOLD=1 build turns vj_decoder_set_hold() on for every decoder.
if(VJ_BENCH_HOLD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VJ_BENCH_HOLD)
endif()
