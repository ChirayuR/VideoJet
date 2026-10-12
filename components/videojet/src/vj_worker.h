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
 * 0 = nothing to do (not armed), 2 = armed but blocked (ring full): poll. Called often, lock free. With stalled != 0 (the
 * worker polled a blocked step for about 200 us) a blocked step announces a sleep and re-checks that it is still blocked:
 * it then returns 3 and the worker blocks until vj_worker_wake() (or a post, a kick, quit); if the block was gone it
 * returns 1 or 2 as usual and the worker does not sleep. */
typedef int (*vj_worker_step_fn)(void *arg, int stalled);

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
/* Wake the task after it announced a sleep (step returned 3): the owner freed a slot. */
void vj_worker_wake(vj_worker_t *w);
/* Counters for tests and profile builds: kicks, sleeps (the worker blocked on a full ring), wakes. */
void vj_worker_stats(vj_worker_t *w, unsigned *kicks, unsigned *sleeps, unsigned *wakes);
/* Busy-wait, for the VJ_PIPE_STALL_EVERY test hook (the decoding task stalls so the ring stays full). */
void vj_stall_us(unsigned us);
/* The task runs on another core than the caller (always 1 on the host). */
int vj_worker_parallel(vj_worker_t *w);
/* A free-running 32-bit microsecond clock, the same on every core and under frequency scaling (esp_timer on ESP; wraps:
 * compare with unsigned subtraction). The take-back timeout runs on it, because the decoding task may not be pinned. */
uint32_t vj_ticks(void);
#define VJ_TICKS_PER_MS 1000u
/* A pause inside a short spin loop, and the pause once a wait has gone on too long to be a spin: the caller sleeps a tick
 * (ESP) so that a worker task on the caller's own core, at a lower or equal priority, can run. */
void vj_relax(void);
void vj_yield_wait(void);
/* The owner waits until *progress >= need, a counter that the worker's fn publishes (store-release) as it goes: a short
 * spin, then a tick's sleep per check. After VJ_WORKER_WAIT_MS it takes the posted run back as vj_worker_wait does (the
 * worker is not scheduled), so it returns when the worker has done everything posted. The owner must have posted the work
 * that makes the counter reach need. */
void vj_worker_wait_progress(vj_worker_t *w, const unsigned *progress, unsigned need);
/* One line on the log, the first time only (the worker is a best-effort speedup: what it cannot do it says once). */
void vj_worker_warn(const char *msg);
#ifndef ESP_PLATFORM
/* Host tests: make vj_worker_create() fail (the thread-creation-failure path); count runs the owner took back. */
extern int vj_worker_test_fail_create;
extern unsigned vj_worker_test_takebacks;
#endif
