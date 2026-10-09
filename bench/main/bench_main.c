/* VideoJet board bench: decode every stream in the `streams` partition, time each frame,
 * check each frame's hash against its golden, print one "VJ ..." line per stream.
 * Hashing and the per-frame yield happen outside the timed region. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "h264bsd_decoder.h"
#include "h264bsd_util.h"
#include "vj_hash.h"

#define VJ_RUNS 2

typedef struct {
    char name[24];
    uint32_t set; /* 0 = search, 1 = held-out */
    uint32_t data_off, data_len, frames, golden_off;
} vj_entry_t; /* 44 bytes, matches "<24s5I" in tools/pack_streams.py */

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
    storage_t *dec = NULL;
    if (!data || !golden || !t) {
        printf("VJ run=%d set=%s stream=%s error=nomem\n", run, set, e->name);
        goto out;
    }
    ESP_ERROR_CHECK(esp_partition_read(part, e->data_off, data, e->data_len));
    ESP_ERROR_CHECK(esp_partition_read(part, e->golden_off, golden, e->frames * 4));
    dec = h264bsdAlloc();
    if (!dec || h264bsdInit(dec, HANTRO_TRUE) != HANTRO_OK) {
        printf("VJ run=%d set=%s stream=%s error=init\n", run, set, e->name);
        goto out;
    }

    uint8_t *p = data;
    uint32_t left = e->data_len, n = 0, fails = 0, stalls = 0;
    int first_fail = -1;
    const char *err = NULL;
    int64_t t0 = esp_timer_get_time();
    while (left > 0 && n < e->frames) {
        /* Anything whose address goes to the h264bsd API is u32 (unsigned int): uint32_t is
         * unsigned long on Xtensa, an incompatible pointer type that GCC 14 rejects. */
        u32 used = 0;
        uint32_t r = h264bsdDecode(dec, p, left, 0, &used);
        p += used;
        left -= used;
        if (r == H264BSD_ERROR || r == H264BSD_PARAM_SET_ERROR || r == H264BSD_MEMALLOC_ERROR) {
            err = "decode";
            break;
        }
        if (used == 0 && r != H264BSD_PIC_RDY && ++stalls > 2) {
            err = "stall";
            break;
        }
        if (used)
            stalls = 0;
        if (r != H264BSD_PIC_RDY)
            continue;
        t[n] = esp_timer_get_time() - t0; /* this frame's decode time, deblocking included */

        u32 id, idr, nerr, crop, x0, w, y0, h;
        uint8_t *pic = h264bsdNextOutputPicture(dec, &id, &idr, &nerr);
        if (!pic) {
            err = "nopic";
            break;
        }
        uint32_t W = h264bsdPicWidth(dec) * 16, H = h264bsdPicHeight(dec) * 16;
        h264bsdCroppingParams(dec, &crop, &x0, &w, &y0, &h);
        if (!crop) {
            x0 = 0;
            y0 = 0;
            w = W;
            h = H;
        }
        const uint8_t *U = pic + W * H, *V = U + (W / 2) * (H / 2);
        if (vj_hash_i420(pic, W, U, V, W / 2, x0, y0, w, h) != golden[n]) {
            fails++;
            if (first_fail < 0)
                first_fail = (int)n;
        }
        n++;
        vTaskDelay(1); /* let IDLE0 feed the task watchdog */
        t0 = esp_timer_get_time();
    }

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
out:
    if (dec) {
        h264bsdShutdown(dec);
        h264bsdFree(dec);
    }
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
