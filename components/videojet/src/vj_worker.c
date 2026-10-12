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

#include "esp_cpu.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

/* Stack bytes (internal RAM; IDF allocates task stacks there). Measured high water: the deblock filter alone 988 B (2 KB, as
 * before the parse pipeline); with the parser on the same task 1,152 B on every bench stream (VJ_PROFILE builds log it at
 * destroy), so 3 KB leaves the margin for an interrupt frame and the paths the streams do not reach. With VJ_ERROR_PRINT the
 * parser prints from this task (printf needs about 1.5 KB more; not measured), hence 4 KB. */
#if defined(VJ_PARSE_PIPELINE) && defined(CONFIG_VJ_ERROR_PRINT)
#define VJ_WORKER_STACK 4096
#elif defined(VJ_PARSE_PIPELINE)
#define VJ_WORKER_STACK 3072
#else
#define VJ_WORKER_STACK 2048
#endif
/* An armed worker with nothing to do (the ring is full) polls this long (CPU cycles, about 200 us), then announces a
 * sleep and blocks until the decoding task, which frees a slot, wakes it (vj_worker_wake: a notify only when the worker
 * announced the sleep, so no kernel call per macroblock). A fixed constant: the ring holds 6 macroblocks, the decoding
 * task frees one every 10-100 us, so the poll only ends when the decoding task itself stalls. */
#define VJ_STALL_CYCLES (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 200u)

struct vj_worker {
    vj_worker_fn fn;
    void *arg;
    vj_worker_step_fn step;
    void *sarg;
    TaskHandle_t task;
    SemaphoreHandle_t idle;   /* given by the worker each time it drains its posts */
    SemaphoreHandle_t exited; /* given once, just before the task deletes itself */
    portMUX_TYPE lock;        /* guards seq, upto, hseq, running, quit: its enter/exit are the barriers the header promises */
    uint32_t seq, upto;       /* owner: bumped per post */
    uint32_t hseq;            /* the last seq handled */
    int running;              /* a run (worker's, or one the owner took back) is between its claim and its end */
    int quit;
    int core;
    unsigned nkick, nsleep, nwake; /* counters (relaxed atomics): tests and profile builds */
};

uint32_t vj_ticks(void)
{
    return (uint32_t)esp_timer_get_time();
}

void vj_relax(void)
{
    __asm__ __volatile__("nop");
}

void vj_yield_wait(void)
{
    vTaskDelay(1);
}

static void warn_once(const char *msg, int *flag)
{
    if (!*flag) {
        *flag = 1;
        ESP_LOGW("vjdbw", "%s", msg);
    }
}

/* One cooperative loop: parse when the ring has room (never preempted by deblock), else a deblock segment of a claimed
 * run, else poll an armed parser for 200 us, else sleep (a blocked ring: until the decoding task frees a slot; nothing
 * armed: until a post or an arm). One task, so there is no kernel wake per macroblock. The task is runnable for as long
 * as it parses, filters or polls, i.e. through each slice it parses; lower-priority tasks on its core run in its gaps. */
static void worker_main(void *p)
{
    struct vj_worker *w = p;
    uint32_t s = 0, u = 0, t0 = esp_cpu_get_cycle_count();
    int active = 0;
    for (;;) {
        vj_worker_step_fn stepf = __atomic_load_n(&w->step, __ATOMIC_ACQUIRE);
        int r = stepf ? stepf(w->sarg, 0) : 0;
        if (r == 1) {
            t0 = esp_cpu_get_cycle_count();
            continue;
        }
        if (active) {
            if (w->fn(w->arg, u)) {
                portENTER_CRITICAL(&w->lock);
                w->hseq = s;
                w->running = 0;
                portEXIT_CRITICAL(&w->lock);
                xSemaphoreGive(w->idle);
                active = 0;
            }
            t0 = esp_cpu_get_cycle_count();
            continue;
        }
        /* a run to claim? When armed and blocked with nothing posted or asked, skip the lock (the owner takes it for
         * every post) */
        if (r != 2 || __atomic_load_n(&w->quit, __ATOMIC_RELAXED) ||
            __atomic_load_n(&w->seq, __ATOMIC_RELAXED) != __atomic_load_n(&w->hseq, __ATOMIC_RELAXED)) {
            int q, claimed;
            portENTER_CRITICAL(&w->lock);
            q = w->quit;
            claimed = !q && w->seq != w->hseq && !w->running;
            if (claimed) {
                s = w->seq;
                u = w->upto;
                w->running = 1;
            }
            portEXIT_CRITICAL(&w->lock);
            if (q) {
                xSemaphoreGive(w->exited);
                vTaskDelete(NULL); /* does not return; w may be freed from here on */
            }
            if (claimed) {
                active = 1;
                continue;
            }
        }
        if (r == 2) { /* armed and blocked: poll, and after 200 us without a slot freed, sleep until one is */
            if (esp_cpu_get_cycle_count() - t0 >= VJ_STALL_CYCLES) {
                if (stepf(w->sarg, 1) == 3) {
                    __atomic_fetch_add(&w->nsleep, 1, __ATOMIC_RELAXED);
                    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                }
                t0 = esp_cpu_get_cycle_count();
            }
            continue;
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        t0 = esp_cpu_get_cycle_count();
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
    w->core = (int)core;
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

void vj_worker_set_step(vj_worker_t *w, vj_worker_step_fn f, void *arg)
{
    w->sarg = arg; /* before the pointer: the task reads step first */
    __atomic_store_n(&w->step, f, __ATOMIC_RELEASE);
}

void vj_worker_kick(vj_worker_t *w)
{
    __atomic_fetch_add(&w->nkick, 1, __ATOMIC_RELAXED);
    xTaskNotifyGive(w->task);
}

void vj_worker_wake(vj_worker_t *w)
{
    __atomic_fetch_add(&w->nwake, 1, __ATOMIC_RELAXED);
    xTaskNotifyGive(w->task);
}

void vj_worker_stats(vj_worker_t *w, unsigned *kicks, unsigned *sleeps, unsigned *wakes)
{
    *kicks = __atomic_load_n(&w->nkick, __ATOMIC_RELAXED);
    *sleeps = __atomic_load_n(&w->nsleep, __ATOMIC_RELAXED);
    *wakes = __atomic_load_n(&w->nwake, __ATOMIC_RELAXED);
}

void vj_stall_us(unsigned us)
{
    esp_rom_delay_us(us);
}

int vj_worker_parallel(vj_worker_t *w)
{
    return w->core != (int)xPortGetCoreID();
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
            while (!w->fn(w->arg, u))
                ;
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
#ifdef CONFIG_VJ_PROFILE /* profiling builds: how much of the worker's stack the deblock filter and the parser used */
    ESP_LOGI("vjdbw", "worker stack: %u of %u B used (high water); slice arms %u, ring-full sleeps %u, wakes %u",
        (unsigned)(VJ_WORKER_STACK - uxTaskGetStackHighWaterMark(w->task)), (unsigned)VJ_WORKER_STACK, w->nkick, w->nsleep,
        w->nwake);
#endif
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
#include <sched.h>
#include <time.h>
#include <unistd.h>

int vj_worker_test_fail_create;
unsigned vj_worker_test_takebacks;

struct vj_worker {
    vj_worker_fn fn;
    void *arg;
    vj_worker_step_fn step;
    void *sarg;
    pthread_t th;
    pthread_mutex_t m;
    pthread_cond_t work, idle;
    uint32_t seq, upto, hseq;
    int running;
    int quit;
    int kicked;
    unsigned nkick, nsleep, nwake;
};

uint32_t vj_ticks(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u);
}

void vj_relax(void)
{
    sched_yield();
}

void vj_yield_wait(void)
{
    usleep(50);
}

/* The same cooperative loop as the ESP task: parse step, deblock segment, poll 200 us, sleep. */
static void *worker_main(void *p)
{
    struct vj_worker *w = p;
    uint32_t s = 0, u = 0, stalled = 0, t0 = vj_ticks();
    int active = 0;
    for (;;) {
        vj_worker_step_fn stepf = __atomic_load_n(&w->step, __ATOMIC_ACQUIRE);
        int r = stepf ? stepf(w->sarg, 0) : 0;
        if (r == 1) {
            t0 = vj_ticks();
            continue;
        }
        if (active) {
            if (w->fn(w->arg, u)) {
                pthread_mutex_lock(&w->m);
                w->hseq = s;
                w->running = 0;
                pthread_cond_broadcast(&w->idle);
                pthread_mutex_unlock(&w->m);
                active = 0;
            }
            t0 = vj_ticks();
            continue;
        }
        pthread_mutex_lock(&w->m);
        if (w->quit) {
            pthread_mutex_unlock(&w->m);
            break;
        }
        if (w->seq != w->hseq && !w->running) {
#ifdef VJ_WORKER_STALL_MS /* test: a worker that is not scheduled for a while before it claims the run */
            if (stalled != w->seq) {
                stalled = w->seq;
                pthread_mutex_unlock(&w->m);
                usleep(VJ_WORKER_STALL_MS * 1000);
                continue;
            }
#endif
            (void)stalled;
            s = w->seq;
            u = w->upto;
            w->running = 1;
            active = 1;
            pthread_mutex_unlock(&w->m);
#ifdef VJ_WORKER_JITTER_US /* race stress: the worker dawdles a random time before each run */
            usleep((unsigned)rand() % VJ_WORKER_JITTER_US);
#endif
            continue;
        }
        if (w->kicked) {
            w->kicked = 0;
            pthread_mutex_unlock(&w->m);
            continue;
        }
        if (r == 2) { /* armed and blocked: poll, and after 200 us without a slot freed, sleep until one is */
            pthread_mutex_unlock(&w->m);
            if (vj_ticks() - t0 >= 200) {
                if (stepf(w->sarg, 1) == 3) {
                    __atomic_fetch_add(&w->nsleep, 1, __ATOMIC_RELAXED);
                    pthread_mutex_lock(&w->m);
                    while (!w->quit && !w->kicked && !(w->seq != w->hseq && !w->running))
                        pthread_cond_wait(&w->work, &w->m);
                    w->kicked = 0;
                    pthread_mutex_unlock(&w->m);
                }
                t0 = vj_ticks();
            } else
                sched_yield();
            continue;
        }
        pthread_cond_wait(&w->work, &w->m);
        pthread_mutex_unlock(&w->m);
        t0 = vj_ticks();
    }
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

void vj_worker_set_step(vj_worker_t *w, vj_worker_step_fn f, void *arg)
{
    w->sarg = arg;
    __atomic_store_n(&w->step, f, __ATOMIC_RELEASE);
}

void vj_worker_kick(vj_worker_t *w)
{
    __atomic_fetch_add(&w->nkick, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&w->m);
    w->kicked = 1;
    pthread_cond_signal(&w->work);
    pthread_mutex_unlock(&w->m);
}

void vj_worker_wake(vj_worker_t *w)
{
    __atomic_fetch_add(&w->nwake, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&w->m);
    w->kicked = 1;
    pthread_cond_signal(&w->work);
    pthread_mutex_unlock(&w->m);
}

void vj_worker_stats(vj_worker_t *w, unsigned *kicks, unsigned *sleeps, unsigned *wakes)
{
    *kicks = __atomic_load_n(&w->nkick, __ATOMIC_RELAXED);
    *sleeps = __atomic_load_n(&w->nsleep, __ATOMIC_RELAXED);
    *wakes = __atomic_load_n(&w->nwake, __ATOMIC_RELAXED);
}

void vj_stall_us(unsigned us)
{
    uint32_t t0 = vj_ticks();
    while (vj_ticks() - t0 < us)
        ;
}

int vj_worker_parallel(vj_worker_t *w)
{
    (void)w;
    return 1;
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
        while (!w->fn(w->arg, u))
            ;
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

/* A spin of this long (500 us) is a wait for a piece of work, any longer the worker may share the waiting task's core */
#define VJ_PROGRESS_SPIN_TICKS (500u * VJ_TICKS_PER_MS / 1000u)

void vj_worker_wait_progress(vj_worker_t *w, const unsigned *progress, unsigned need)
{
    uint32_t t0 = 0;
    unsigned spins = 0;
    int started = 0;

    while (__atomic_load_n(progress, __ATOMIC_ACQUIRE) < need) {
        if (!(++spins & 63)) {
            uint32_t now = vj_ticks();

            if (!started) {
                started = 1;
                t0 = now;
            } else if (now - t0 > VJ_WORKER_WAIT_MS * VJ_TICKS_PER_MS) {
                /* no luck for VJ_WORKER_WAIT_MS: the worker is not running. Take the posted run back (a run the worker
                 * has started is waited for) */
                vj_worker_wait(w);
                return;
            } else if (now - t0 > VJ_PROGRESS_SPIN_TICKS) {
                vj_yield_wait();
            }
        }
        vj_relax();
    }
}

#endif /* VJ_DEBLOCK_WORKER */
