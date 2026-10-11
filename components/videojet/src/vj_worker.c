#include "vj_worker.h"
#include <stdlib.h>

#ifdef VJ_DEBLOCK_WORKER

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define VJ_WORKER_STACK 3072 /* bytes; internal RAM (IDF allocates task stacks there) */

struct vj_worker {
    vj_worker_fn fn;
    void *arg;
    TaskHandle_t task;
    SemaphoreHandle_t idle;   /* given by the worker each time it drains its posts */
    SemaphoreHandle_t exited; /* given once, just before the task deletes itself */
    portMUX_TYPE lock;        /* guards seq, upto, hseq, quit: its enter/exit are the barriers the header promises */
    uint32_t seq, upto;       /* owner: bumped per post */
    uint32_t hseq;            /* worker: the last seq handled */
    int quit;
};

static void worker_main(void *p)
{
    struct vj_worker *w = p;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            uint32_t s, u;
            int q;
            portENTER_CRITICAL(&w->lock);
            s = w->seq;
            u = w->upto;
            q = w->quit;
            portEXIT_CRITICAL(&w->lock);
            if (q) {
                xSemaphoreGive(w->exited);
                vTaskDelete(NULL); /* does not return; w may be freed from here on */
            }
            if (s == w->hseq)
                break;
            w->fn(w->arg, u);
            portENTER_CRITICAL(&w->lock);
            w->hseq = s;
            portEXIT_CRITICAL(&w->lock);
        }
        xSemaphoreGive(w->idle);
    }
}

vj_worker_t *vj_worker_create(vj_worker_fn fn, void *arg)
{
    struct vj_worker *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->fn = fn;
    w->arg = arg;
    portMUX_INITIALIZE(&w->lock);
    w->idle = xSemaphoreCreateBinary();
    w->exited = xSemaphoreCreateBinary();
    /* One priority below the creating task, at least 1: the worker never preempts the decoder's own core. */
    UBaseType_t prio = uxTaskPriorityGet(NULL);
    prio = prio > 1 ? prio - 1 : 1;
#if CONFIG_VJ_WORKER_CORE >= 0
    BaseType_t core = CONFIG_VJ_WORKER_CORE;
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

void vj_worker_wait(vj_worker_t *w)
{
    for (;;) {
        portENTER_CRITICAL(&w->lock);
        int done = w->hseq == w->seq;
        portEXIT_CRITICAL(&w->lock);
        if (done)
            return;
        xSemaphoreTake(w->idle, portMAX_DELAY); /* a stale token only costs one more pass of this loop */
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

#else /* host: pthreads */
#include <pthread.h>
#ifdef VJ_WORKER_JITTER_US /* race stress for the host gate: the worker dawdles a random time before each call */
#include <unistd.h>
#endif

struct vj_worker {
    vj_worker_fn fn;
    void *arg;
    pthread_t th;
    pthread_mutex_t m;
    pthread_cond_t work, idle;
    uint32_t seq, upto, hseq;
    int quit;
};

static void *worker_main(void *p)
{
    struct vj_worker *w = p;
    pthread_mutex_lock(&w->m);
    for (;;) {
        while (!w->quit && w->seq == w->hseq)
            pthread_cond_wait(&w->work, &w->m);
        if (w->quit)
            break;
        uint32_t s = w->seq, u = w->upto;
        pthread_mutex_unlock(&w->m);
#ifdef VJ_WORKER_JITTER_US
        usleep((unsigned)rand() % VJ_WORKER_JITTER_US);
#endif
        w->fn(w->arg, u);
        pthread_mutex_lock(&w->m);
        w->hseq = s;
        pthread_cond_broadcast(&w->idle);
    }
    pthread_mutex_unlock(&w->m);
    return NULL;
}

vj_worker_t *vj_worker_create(vj_worker_fn fn, void *arg)
{
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

void vj_worker_wait(vj_worker_t *w)
{
    pthread_mutex_lock(&w->m);
    while (w->hseq != w->seq)
        pthread_cond_wait(&w->idle, &w->m);
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
#endif /* ESP_PLATFORM */

#endif /* VJ_DEBLOCK_WORKER */
