/* Comparison builds (VJ_BENCH_DEC=esp_h264, bench/CMakeLists.txt): Espressif's esp_h264 software decoder behind
 * VideoJet's API, so bench_main.c times and hashes it exactly as it does VideoJet. Each AU goes through as many
 * esp_h264_dec_process() calls as it takes to consume it.
 * esp_h264 reports the coded size only, no SPS cropping: a cropped stream (test_640x360) hashes its coded picture and
 * cannot match the golden. */
#include <stdlib.h>
#include "esp_h264_dec_sw.h"
#include "videojet.h"

struct vj_decoder {
    esp_h264_dec_handle_t h;
    esp_h264_dec_param_sw_handle_t param;
};

void vj_decoder_destroy(vj_decoder_t *dec)
{
    if (!dec)
        return;
    if (dec->h) {
        esp_h264_dec_close(dec->h);
        esp_h264_dec_del(dec->h);
    }
    free(dec);
}

vj_decoder_t *vj_decoder_create(void)
{
    vj_decoder_t *dec = calloc(1, sizeof *dec);
    esp_h264_dec_cfg_sw_t cfg = {.pic_type = ESP_H264_RAW_FMT_I420};
    if (dec && esp_h264_dec_sw_new(&cfg, &dec->h) == ESP_H264_ERR_OK && esp_h264_dec_open(dec->h) == ESP_H264_ERR_OK &&
        esp_h264_dec_sw_get_param_hd(dec->h, &dec->param) == ESP_H264_ERR_OK)
        return dec;
    vj_decoder_destroy(dec);
    return NULL;
}

vj_status_t vj_decode_au(vj_decoder_t *dec, uint8_t *au, size_t len, vj_frame_t *out)
{
    esp_h264_dec_in_frame_t in = {.raw_data = {.buffer = au, .len = len}};
    vj_status_t st = VJ_NO_FRAME;
    while (in.raw_data.len) {
        esp_h264_dec_out_frame_t o = {.out_size = 0};
        esp_h264_err_t err = esp_h264_dec_process(dec->h, &in, &o);
        if (err != ESP_H264_ERR_OK)
            return err == ESP_H264_ERR_MEM ? VJ_ERR_NOMEM : VJ_ERR_STREAM;
        if (o.out_size) {
            esp_h264_resolution_t res;
            esp_h264_dec_get_resolution(dec->param, &res);
            size_t luma = (size_t)res.width * res.height;
            *out = (vj_frame_t){.y = o.outbuf, .u = o.outbuf + luma, .v = o.outbuf + luma + luma / 4,
                                .y_stride = res.width, .uv_stride = res.width / 2, .width = res.width,
                                .height = res.height, .crop_w = res.width, .crop_h = res.height};
            st = VJ_FRAME;
        } else if (!in.consume) {
            return VJ_ERR_STREAM; /* no progress: never spin */
        }
        in.raw_data.buffer += in.consume;
        in.raw_data.len -= in.consume;
    }
    return st;
}

bool vj_profile_take(vj_profile_t *p)
{
    (void)p;
    return false;
}
