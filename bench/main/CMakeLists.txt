idf_component_register(SRCS "bench_main.c" INCLUDE_DIRS "."
                       PRIV_REQUIRES videojet vj_hash esp_partition esp_timer spi_flash)
# The hold-on score: idf.py -DVJ_BENCH_HOLD=1 build turns vj_decoder_set_hold() on for every decoder.
if(VJ_BENCH_HOLD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VJ_BENCH_HOLD)
endif()
