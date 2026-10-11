#include "vj_worker.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef VJ_DEBLOCK_WORKER

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define VJ_WORKER_STACK 2048 /* bytes; internal RAM (IDF allocates task stacks there). Measured use: 988 B */

struct vj_worker {
    vj_worker_fn fn;
    void *arg;
    TaskHandle_t task;
    SemaphoreHandle_t idle;   /* given by the worker each time it drains its posts */
    SemaphoreHandle_t exited; /* given once, just before the task deletes itself */
    portMUX_TYPE lock;        /* guards seq, upto, hseq, running, quit: its enter/exit are the barriers the header promises */
    uint32_t seq, upto;       /* owner: bumped per post */
    uint32_t hseq;            /* the last seq handled */
    int running;              /* a run (worker's, or one the owner took back) is between its claim and its end */
    int quit;
};

static void warn_once(const char *msg, int *flag)
{
    if (!*flag) {
        *flag = 1;
        ESP_LOGW("vjdbw", "%s", msg);
    }
}

static void worker_main(void *p)
{
    struct vj_worker *w = p;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            uint32_t s, u;
            int q, claimed;
            portENTER_CRITICAL(&w->lock);
            s = w->seq;
            u = w->upto;
            q = w->quit;
            claimed = !q && s != w->hseq && !w->running;
            if (claimed)
                w->running = 1;
            portEXIT_CRITICAL(&w->lock);
            if (q) {
                xSemaphoreGive(w->exited);
                vTaskDelete(NULL); /* does not return; w may be freed from here on */
            }
            if (!claimed)
                break; /* nothing posted, or the owner took the run back */
            w->fn(w->arg, u);
            portENTER_CRITICAL(&w->lock);
            w->hseq = s;
            w->running = 0;
            portEXIT_CRITICAL(&w->lock);
            xSemaphoreGive(w->idle);
        }
    }
}

vj_worker_t *vj_worker_create(vj_worker_fn fn, void *arg)
{
    /* the portMUX must live in internal RAM */
    struct vj_worker *w = heap_caps_calloc(1, sizeof *w, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!w)
        return NULL;
    w->fn = fn;
    w->arg = arg;
    portMUX_INITIALIZE(&w->lock);
    w->idle = xSemaphoreCreateBinary();
    w->exited = xSemaphoreCreateBinary();
    /* The creating task's own priority: a task on the worker's core below it cannot starve the worker, and if one at
     * or above it does, vj_worker_wait() takes the run back after VJ_WORKER_WAIT_MS. */
    UBaseType_t prio = uxTaskPriorityGet(NULL);
#if CONFIG_VJ_WORKER_CORE >= 0
    BaseType_t core = CONFIG_VJ_WORKER_CORE < portNUM_PROCESSORS ? CONFIG_VJ_WORKER_CORE : 0; /* single-core: 0 */
#else
    BaseType_t core = portNUM_PROCESSORS > 1 ? 1 - xPortGetCoreID() : 0; /* the core the creating task is not on */
#endif
    if (!w->idle || !w->exited ||
        xTaskCreatePinnedToCore(worker_main, "vjdbw", VJ_WORKER_STACK, w, prio, &w->task, core) != pdPASS) {
        if (w->idle)
            vSemaphoreDelete(w->idle);
        if (w->exited)
            vSemaphoreDelete(w->exited);
        free(w);
        return NULL;
    }
    return w;
}

void vj_worker_post(vj_worker_t *w, uint32_t upto)
{
    portENTER_CRITICAL(&w->lock);
    w->upto = upto;
    w->seq++;
    portEXIT_CRITICAL(&w->lock);
    xTaskNotifyGive(w->task);
}

int vj_worker_busy(vj_worker_t *w)
{
    portENTER_CRITICAL(&w->lock);
    int b = w->hseq != w->seq;
    portEXIT_CRITICAL(&w->lock);
    return b;
}

void vj_worker_wait(vj_worker_t *w)
{
    static int warned;
    for (;;) {
        uint32_t s = 0, u = 0;
        int done, take = 0;
        portENTER_CRITICAL(&w->lock);
        done = w->hseq == w->seq;
        portEXIT_CRITICAL(&w->lock);
        if (done)
            return;
        if (xSemaphoreTake(w->idle, pdMS_TO_TICKS(VJ_WORKER_WAIT_MS)) == pdTRUE)
            continue; /* a stale token only costs one more pass of this loop */
        /* The worker has not finished in time. If it has not even started the run, do it here (once: the claim is
         * the flag the worker tests); if it is in the middle of it, keep waiting. */
        portENTER_CRITICAL(&w->lock);
        if (w->hseq != w->seq && !w->running) {
            w->running = 1;
            s = w->seq;
            u = w->upto;
            take = 1;
        }
        portEXIT_CRITICAL(&w->lock);
        if (take) {
            warn_once("deblock worker not scheduled: the decoding task ran its work (a task on the worker's core "
                      "at or above its priority?)", &warned);
            w->fn(w->arg, u);
            portENTER_CRITICAL(&w->lock);
            w->hseq = s;
            w->running = 0;
            portEXIT_CRITICAL(&w->lock);
        }
    }
}

void vj_worker_destroy(vj_worker_t *w)
{
    if (!w)
        return;
    vj_worker_wait(w);
    portENTER_CRITICAL(&w->lock);
    w->quit = 1;
    portEXIT_CRITICAL(&w->lock);
    xTaskNotifyGive(w->task);
    xSemaphoreTake(w->exited, portMAX_DELAY);
    vSemaphoreDelete(w->idle);
    vSemaphoreDelete(w->exited);
    free(w);
}

void vj_worker_warn(const char *msg)
{
    static int warned;
    warn_once(msg, &warned);
}

#else /* host: pthreads */
#include <pthread.h>
#include <time.h>
#include <unistd.h>

int vj_worker_test_fail_create;
unsigned vj_worker_test_takebacks;

struct vj_worker {
    vj_worker_fn fn;
    void *arg;
    pthread_t th;
    pthread_mutex_t m;
    pthread_cond_t work, idle;
    uint32_t seq, upto, hseq;
    int running;
    int quit;
};

static void *worker_main(void *p)
{
    struct vj_worker *w = p;
    pthread_mutex_lock(&w->m);
    for (;;) {
        while (!w->quit && (w->seq == w->hseq || w->running))
            pthread_cond_wait(&w->work, &w->m);
        if (w->quit)
            break;
#ifdef VJ_WORKER_STALL_MS /* test: a worker that is not scheduled for a while before it claims the run */
        pthread_mutex_unlock(&w->m);
        usleep(VJ_WORKER_STALL_MS * 1000);
        pthread_mutex_lock(&w->m);
        if (w->quit)
            break;
        if (w->seq == w->hseq || w->running)
            continue;
#endif
        uint32_t s = w->seq, u = w->upto;
        w->running = 1;
        pthread_mutex_unlock(&w->m);
#ifdef VJ_WORKER_JITTER_US /* race stress: the worker dawdles a random time before each run */
        usleep((unsigned)rand() % VJ_WORKER_JITTER_US);
#endif
        w->fn(w->arg, u);
        pthread_mutex_lock(&w->m);
        w->hseq = s;
        w->running = 0;
        pthread_cond_broadcast(&w->idle);
    }
    pthread_mutex_unlock(&w->m);
    return NULL;
}

vj_worker_t *vj_worker_create(vj_worker_fn fn, void *arg)
{
    if (vj_worker_test_fail_create)
        return NULL;
    struct vj_worker *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->fn = fn;
    w->arg = arg;
    pthread_mutex_init(&w->m, NULL);
    pthread_cond_init(&w->work, NULL);
    pthread_cond_init(&w->idle, NULL);
    if (pthread_create(&w->th, NULL, worker_main, w)) {
        free(w);
        return NULL;
    }
    return w;
}

void vj_worker_post(vj_worker_t *w, uint32_t upto)
{
    pthread_mutex_lock(&w->m);
    w->upto = upto;
    w->seq++;
    pthread_cond_signal(&w->work);
    pthread_mutex_unlock(&w->m);
}

int vj_worker_busy(vj_worker_t *w)
{
    pthread_mutex_lock(&w->m);
    int b = w->hseq != w->seq;
    pthread_mutex_unlock(&w->m);
    return b;
}

void vj_worker_wait(vj_worker_t *w)
{
    pthread_mutex_lock(&w->m);
    while (w->hseq != w->seq) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_nsec += VJ_WORKER_WAIT_MS * 1000000L;
        t.tv_sec += t.tv_nsec / 1000000000L;
        t.tv_nsec %= 1000000000L;
        if (pthread_cond_timedwait(&w->idle, &w->m, &t) == 0 || w->hseq == w->seq)
            continue;
#ifndef VJ_WORKER_MUTANT_TAKE_RUNNING /* mutant: take the run back even when the worker is in the middle of it */
        if (w->running)
            continue;
#endif
        uint32_t s = w->seq, u = w->upto;
        w->running = 1;
        vj_worker_test_takebacks++;
        pthread_mutex_unlock(&w->m);
        w->fn(w->arg, u);
        pthread_mutex_lock(&w->m);
        w->hseq = s;
        w->running = 0;
        pthread_cond_broadcast(&w->idle);
    }
    pthread_mutex_unlock(&w->m);
}

void vj_worker_destroy(vj_worker_t *w)
{
    if (!w)
        return;
    vj_worker_wait(w);
    pthread_mutex_lock(&w->m);
    w->quit = 1;
    pthread_cond_signal(&w->work);
    pthread_mutex_unlock(&w->m);
    pthread_join(w->th, NULL);
    pthread_mutex_destroy(&w->m);
    pthread_cond_destroy(&w->work);
    pthread_cond_destroy(&w->idle);
    free(w);
}

void vj_worker_warn(const char *msg)
{
    static int warned;
    if (!warned) {
        warned = 1;
        fprintf(stderr, "vjdbw: %s\n", msg);
    }
}
#endif /* ESP_PLATFORM */

#endif /* VJ_DEBLOCK_WORKER */
