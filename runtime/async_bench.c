/*===- async_bench.c: a wake-storm microbenchmark for the netasync poller.
 *
 * Not part of libpascalrt: build it standalone, next to the runtime, and
 * run it on the machine being measured (it exercises io_uring on Linux,
 * epoll when the kernel has none, kqueue on macOS):
 *
 *   gcc -O2 -std=gnu11 -pthread -I include async.c async_bench.c -o poller
 *   ./poller [threads] [pipes per thread] [payload count]
 *
 * `threads` pool worker threads (P3_POOL_THREADS) get, round robin, one
 * task per pipe; each task parks on its pipe's read side through one
 * __pas_task_wait_fd per payload, so every payload is a full poller round
 * trip: submit, poller deliver, cross-thread post to the task's thread,
 * resume, read. The main thread (plain context, no task) pumps 8-byte
 * payloads into the pipes until the target count, then stops the tasks.
 * The report: payloads/s and the write-to-read latency of each payload —
 * note the latency includes queueing while a task is behind the pump.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "pascalrt.h"

static long long g_payloads;
static long long g_bucks[64];
static int g_stop;   /* read with __atomic_load_n */

typedef struct {
  int rfd;
} WorkerArgs;

static int64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static void bump_lat(int64_t ns) {
  int64_t us = ns / 1000;
  int b = 0;
  while ((2LL << b) <= us && b < 62)
    b++;
  __atomic_fetch_add(&g_bucks[b], 1, __ATOMIC_RELAXED);
}

static void worker_entry(PasTask *t) {
  WorkerArgs *a = __pas_task_args(t);
  int64_t buf[64];
  for (;;) {
    if (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE))
      break;
    __pas_task_wait_fd(a->rfd, 0);
    ssize_t n = read(a->rfd, buf, (int)sizeof buf);
    if (n <= 0)
      continue;
    int64_t end = now_ns();
    int cnt = (int)(n / (ssize_t)sizeof buf[0]);
    __atomic_fetch_add(&g_payloads, cnt, __ATOMIC_RELAXED);
    for (int i = 0; i < cnt; i++)
      bump_lat(end - buf[i]);
  }
}

int main(int argc, char **argv) {
  int threads = argc > 1 ? atoi(argv[1]) : 2;
  int pipes = argc > 2 ? atoi(argv[2]) : 128;
  long target = argc > 3 ? atol(argv[3]) : 2000000;
  if (threads < 1)
    threads = 1;
  if (pipes < 1)
    pipes = 1;
  char env[16];
  snprintf(env, sizeof env, "%d", threads);
  setenv("P3_POOL_THREADS", env, 1);

  int *rfd = malloc((size_t)pipes * sizeof(int));
  int *wfd = malloc((size_t)pipes * sizeof(int));
  for (int i = 0; i < pipes; i++) {
    int p[2];
    if (pipe(p) != 0)
      return 1;
    rfd[i] = p[0];
    wfd[i] = p[1];
  }

  WorkerArgs a;
  for (int i = 0; i < pipes; i++) {
    a.rfd = rfd[i];
    PasTask *t = __pas_task_new(NULL, sizeof(WorkerArgs), worker_entry, NULL);
    memcpy(__pas_task_args(t), &a, sizeof a);
    __pas_task_spawn(t);
    __pas_task_release(t);
  }

  int64_t t0 = now_ns();
  long written = 0;
  while (written < target) {
    int64_t stamp = now_ns();
    for (int i = 0; i < pipes && written < target; i++) {
      if (write(wfd[i], &stamp, sizeof stamp) < 0)
        return 1;
      written++;
    }
  }
  __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
  for (int i = 0; i < pipes; i++)
    (void)!write(wfd[i], "stop!", 1);

  /* the tasks finish their last payloads */
  for (int wait = 0; wait < 100; wait++) {
    int64_t p = __atomic_load_n(&g_payloads, __ATOMIC_RELAXED);
    struct timespec ts = {0, 10000000};
    nanosleep(&ts, NULL);
    if (__atomic_load_n(&g_payloads, __ATOMIC_RELAXED) == p)
      break;
  }

  int64_t t1 = now_ns();
  int64_t total = __atomic_load_n(&g_payloads, __ATOMIC_RELAXED);
  double sec = (double)(t1 - t0) / 1e9;
  long long b50 = 0, b99 = 0, run = 0;
  int i;
  for (i = 0; i < 64; i++) {
    run += __atomic_load_n(&g_bucks[i], __ATOMIC_RELAXED);
    if (run < total / 2)
      b50 = 1LL << i;
    if (run < total * 99 / 100)
      b99 = 1LL << i;
  }
  const char *backend = "kevent";
  int32_t bk = __pas_net_fd_backend();
  if (bk == 1)
    backend = "epoll";
  else if (bk == 2)
    backend = "io_uring";
  printf("backend=%s threads=%d pipes=%d\n", backend, threads, pipes);
  printf("payloads=%lld in %.3fs: %.0f wakes/s\n",
         (long long)total, sec, (double)total / sec);
  printf("latency write->read: p50=%lldus p99=%lldus (includes queueing)\n",
         b50, b99);
  return 0;
}
