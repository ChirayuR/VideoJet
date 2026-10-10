/* VideoJet board bench: decode every stream in the `streams` partition through the public API, one access unit per
 * vj_decode_au() call, time each frame, check each frame's hash against its golden, print one "VJ ..." line per
 * stream. Hashing and the per-frame yield happen outside the timed region.
 * A CONFIG_VJ_PROFILE build (see bench/sdkconfig.profile) also prints a "VJP ..." line after each "VJ ..." line. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "videojet.h"
#include "vj_hash.h"

#define VJ_RUNS 2

typedef struct {
    char name[24];
    uint32_t set; /* 0 = search, 1 = held-out */
    uint32_t data_off, data_len, frames, golden_off;
} vj_entry_t; /* 44 bytes, matches "<24s5I" in tools/pack_streams.py */
_Static_assert(sizeof(vj_entry_t) == 44, "must match <24s5I> in tools/pack_streams.py");

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static void run_stream(const esp_partition_t *part, const vj_entry_t *e, int run)
{
    const char *set = e->set ? "heldout" : "search";
    uint8_t *data = heap_caps_malloc(e->data_len, MALLOC_CAP_SPIRAM);
    uint32_t *golden = heap_caps_malloc(e->frames * 4, MALLOC_CAP_SPIRAM);
    int64_t *t = heap_caps_malloc(e->frames * sizeof(int64_t), MALLOC_CAP_SPIRAM);
    vj_decoder_t *dec = NULL;
    uint32_t *au_off = NULL; /* per access unit: offset, length */
    if (!data || !golden || !t) {
        printf("VJ run=%d set=%s stream=%s error=nomem\n", run, set, e->name);
        goto out;
    }
    ESP_ERROR_CHECK(esp_partition_read(part, e->data_off, data, e->data_len));
    ESP_ERROR_CHECK(esp_partition_read(part, e->golden_off, golden, e->frames * 4));
    /* Split the stream into access units once, outside the timed loop. */
    size_t nau = 0;
    for (size_t off = 0, n; off < e->data_len; off += n, nau++)
        if (!(n = vj_au_length(data + off, e->data_len - off)))
            break;
    au_off = heap_caps_malloc((nau ? nau : 1) * 2 * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!au_off) {
        printf("VJ run=%d set=%s stream=%s error=nomem\n", run, set, e->name);
        goto out;
    }
    size_t used = 0; /* bytes covered by the access units found */
    for (size_t i = 0; i < nau; i++) {
        size_t n = vj_au_length(data + used, e->data_len - used);
        au_off[2 * i] = (uint32_t)used;
        au_off[2 * i + 1] = (uint32_t)n;
        used += n;
    }
    dec = vj_decoder_create();
    if (!dec) {
        printf("VJ run=%d set=%s stream=%s error=init\n", run, set, e->name);
        goto out;
    }

    uint32_t n = 0, fails = 0;
    int first_fail = -1;
    const char *err = used < e->data_len ? "noau" : NULL; /* bytes left that hold no start code */
    vj_profile_t prof;
    vj_profile_take(&prof); /* zeroes the counters, when profiling is compiled in */
    int64_t acc = 0; /* time of AUs since the last frame: a parameter-set-only AU counts toward the next frame */
    for (size_t i = 0; i < nau && n < e->frames; i++) {
        vj_frame_t f;
        int64_t t0 = esp_timer_get_time();
        vj_status_t st = vj_decode_au(dec, data + au_off[2 * i], au_off[2 * i + 1], &f);
        acc += esp_timer_get_time() - t0;
        if (st < 0) {
            err = st == VJ_ERR_NOMEM ? "nomem" : "decode";
            break;
        }
        if (st == VJ_NO_FRAME)
            continue;
        t[n] = acc; /* this frame's decode time, deblocking included */
        acc = 0;
        if (vj_hash_i420(f.y, f.y_stride, f.u, f.v, f.uv_stride, f.crop_x, f.crop_y, f.crop_w, f.crop_h)
            != golden[n]) {
            fails++;
            if (first_fail < 0)
                first_fail = (int)n;
        }
        n++;
        vTaskDelay(1); /* let IDLE0 feed the task watchdog: outside the timed region, as in M1 */
    }

    /* Stats: idr_us is the timed sum of the AUs up to and including the first frame (parameter sets included), so it
     * covers SPS/PPS parsing and DPB allocation. p95 is floor-rank s[m * 95 / 100]: it equals max_us for 20 or fewer
     * P-frames. */
    int64_t idr_us = n ? t[0] : 0, sum = 0;
    uint32_t m = n > 1 ? n - 1 : 0;
    int64_t *s = t + 1; /* P-frame times */
    for (uint32_t i = 0; i < m; i++)
        sum += s[i];
    qsort(s, m, sizeof(int64_t), cmp_i64);
    double fps = (sum + idr_us) ? n * 1e6 / (double)(sum + idr_us) : 0.0;
    printf("VJ run=%d set=%s stream=%s frames=%u/%u fail=%u first_fail=%d idr_us=%lld "
           "p_mean_us=%lld p50_us=%lld p95_us=%lld max_us=%lld fps=%.2f int_min=%u psram_min=%u%s%s\n",
           run, set, e->name, (unsigned)n, (unsigned)e->frames, (unsigned)fails, first_fail,
           (long long)idr_us, (long long)(m ? sum / m : 0), (long long)(m ? s[m / 2] : 0),
           (long long)(m ? s[(m * 95) / 100] : 0), (long long)(m ? s[m - 1] : 0), fps,
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
           err ? " error=" : "", err ? err : "");
    if (vj_profile_take(&prof) && n) {
        const uint64_t mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
        uint64_t parse = prof.parse / mhz / n, recon = prof.recon / mhz / n;
        uint64_t inter = prof.inter / mhz / n, dblk = prof.deblock / mhz / n;
        uint64_t frame = (uint64_t)((sum + idr_us) / n);
        printf("VJP run=%d stream=%s parse_us=%llu recon_us=%llu inter_us=%llu deblock_us=%llu other_us=%lld\n",
               run, e->name, parse, recon, inter, dblk, (long long)frame - (long long)(parse + recon + dblk));
    }
out:
    vj_decoder_destroy(dec);
    free(au_off);
    free(data);
    free(golden);
    free(t);
}

static void bench_task(void *arg)
{
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "streams");
    uint8_t hdr[8];
    if (!part || esp_partition_read(part, 0, hdr, sizeof hdr) != ESP_OK || memcmp(hdr, "VJS1", 4)) {
        printf("VJ error=no_streams\n");
        vTaskDelete(NULL);
    }
    uint32_t count;
    memcpy(&count, hdr + 4, 4);
    vj_entry_t *ents = malloc(count * sizeof(vj_entry_t));
    ESP_ERROR_CHECK(esp_partition_read(part, 8, ents, count * sizeof(vj_entry_t)));
    printf("VJ start streams=%u\n", (unsigned)count);
    for (uint32_t set = 0; set < 2; set++) {
        for (int run = 0; run < VJ_RUNS; run++)
            for (uint32_t i = 0; i < count; i++)
                if (ents[i].set == set)
                    run_stream(part, &ents[i], run);
        printf(set == 0 ? "VJ search_done\n" : "VJ done\n");
    }
    free(ents);
    vTaskDelete(NULL);
}

void app_main(void)
{
    xTaskCreatePinnedToCore(bench_task, "bench", 16384, NULL, 5, NULL, 0);
}
