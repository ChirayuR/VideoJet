#pragma once
/* One background worker thread for the deblock worker (VJ_DEBLOCK_WORKER): a FreeRTOS task on ESP, a pthread on the
 * host (so the host gate and ThreadSanitizer run the real threaded path).
 *
 * The owner posts a monotone "progress" value and the worker calls fn(arg, upto) with the latest one. Posts coalesce:
 * fn may see only the newest value, and may be called again with a value it already handled (it must keep its own
 * position). vj_worker_wait() returns when every post made before it has been handled. Post and wait are full
 * barriers: what the owner wrote before a post is visible to fn, and what fn wrote is visible to the owner after the
 * wait that follows it. One owner thread at a time; the owner must not call post/wait/destroy concurrently.
 *
 * Starvation: vj_worker_wait() gives the worker VJ_WORKER_WAIT_MS to start a posted run. If it has not started it (its
 * core is busy with a task of at least its priority), the owner takes the run back and executes fn itself, once. A run
 * the worker has started is waited for, however long it takes. */
#include <stdint.h>

#define VJ_WORKER_WAIT_MS 50

typedef struct vj_worker vj_worker_t;
/* Returns 1 when the run up to `upto` is complete, 0 when it wants to be called again with the same upto (the worker
 * may run the parser's step between calls). */
typedef int (*vj_worker_fn)(void *arg, uint32_t upto);
/* The parse pipeline's step (VJ_PARSE_PIPELINE), run by the same task ahead of the deblock run: 1 = did a piece of work,
 * 0 = nothing to do (not armed), 2 = armed but blocked (ring full): poll. Called often, lock free. */
typedef int (*vj_worker_step_fn)(void *arg);

/* NULL when the thread cannot be created. */
vj_worker_t *vj_worker_create(vj_worker_fn fn, void *arg);
void vj_worker_post(vj_worker_t *w, uint32_t upto);
void vj_worker_wait(vj_worker_t *w);
/* Waits for pending posts, stops the thread and frees w. NULL is fine. */
void vj_worker_destroy(vj_worker_t *w);
/* True while a posted run has not finished (for tests: nothing may be in flight when vj_decode_au returns). */
int vj_worker_busy(vj_worker_t *w);
void vj_worker_set_step(vj_worker_t *w, vj_worker_step_fn f, void *arg);
/* Wake the task: a step became possible (the owner armed a slice). */
void vj_worker_kick(vj_worker_t *w);
/* The task runs on another core than the caller (always 1 on the host). */
int vj_worker_parallel(vj_worker_t *w);
/* A free-running 32-bit tick (wraps: compare with unsigned subtraction) and its rate, and a pause for spin loops. */
uint32_t vj_ticks(void);
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#define VJ_TICKS_PER_MS (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000u) /* CPU cycles */
#else
#define VJ_TICKS_PER_MS 1000u /* microseconds */
#endif
void vj_relax(void);
/* One line on the log, the first time only (the worker is a best-effort speedup: what it cannot do it says once). */
void vj_worker_warn(const char *msg);
#ifndef ESP_PLATFORM
/* Host tests: make vj_worker_create() fail (the thread-creation-failure path); count runs the owner took back. */
extern int vj_worker_test_fail_create;
extern unsigned vj_worker_test_takebacks;
#endif
