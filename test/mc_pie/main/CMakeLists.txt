idf_component_register(SRCS "mc_pie_test.c" "rec_c.c"
                       PRIV_INCLUDE_DIRS "../../../components/videojet/h264bsd" "../../../components/videojet/src"
                       PRIV_REQUIRES videojet heap esp_timer esp_psram
                       LDFRAGMENTS "linker.lf")
