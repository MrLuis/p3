/* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
/*===- async_deadline_test.c: in-task deadline waits exercised end to end.
 *
 * Not part of libpascalrt: build standalone next to the runtime and run
 * (io_uring on Linux — this is where the linked TIMEOUT op path runs;
 * kqueue/epoll on the other platforms):
 *
 *   gcc -O2 -std=gnu11 -pthread -I include async.c sys.c io.c ansi.c
 *       format.c async_deadline_test.c -o dltest && ./dltest
 *
 * One task, one quiet pipe: a 30 ms deadline must expire on its own
 * (result 8 = NW_TIMEOUT) without leaking the record, and a 500 ms
 * deadline pre-empted by a byte at ~10 ms must report readable.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "pascalrt.h"

static long long now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000LL + (long long)ts.tv_nsec / 1000;
}

typedef struct {
  int rfd;      /* pipe A: stays quiet, its deadline must expire */
  int rfd_b;    /* pipe B: the main thread beats its deadline */
} Args;

static int g_bits_timeout, g_bits_read;
static long long g_dt_timeout, g_dt_read;
static int g_abandoned;
static atomic_int g_done;

static void task_entry(PasTask *t) {
  Args *a = __pas_task_args(t);
  (void)t;
  long long t0 = now_us();
  g_bits_timeout = __pas_task_wait_fd2(a->rfd, 0, 30);
  g_dt_timeout = now_us() - t0;
  g_abandoned = (int)__pas_net_waits_abandoned();

  /* the main thread wrote to pipe B by now: it beats the 500 ms deadline */
  t0 = now_us();
  g_bits_read = __pas_task_wait_fd2(a->rfd_b, 0, 500);
  g_dt_read = now_us() - t0;
  atomic_store_explicit(&g_done, 1, memory_order_release);
}

int main(void) {
  int pa[2], pb[2];
  if (pipe(pa) != 0 || pipe(pb) != 0)
    return 1;
  Args a;
  a.rfd = pa[0];
  a.rfd_b = pb[0];
  int wfd_b = pb[1];

  PasTask *t = __pas_task_new(NULL, sizeof(Args), task_entry, NULL);
  memcpy(__pas_task_args(t), &a, sizeof a);
  __pas_task_spawn(t);
  __pas_task_release(t);

  /* a byte at ~10 ms beats the task's second, 500 ms deadline on pipe B;
   * pipe A stays quiet for its 30 ms deadline */
  usleep(10000);
  char c = 'x';
  if (write(wfd_b, &c, 1) < 0)
    return 1;

  struct timespec ts = {0, 10000000};   /* 10 ms */
  for (int i = 0; i < 50; i++) {
    nanosleep(&ts, NULL);
    if (atomic_load_explicit(&g_done, memory_order_acquire))
      break;
  }

  const char *backend = "kevent";
  int32_t bk = __pas_net_fd_backend();
  if (bk == 1)
    backend = "epoll";
  else if (bk == 2)
    backend = "io_uring";
  printf("backend=%s\n", backend);
  int ok1 = g_bits_timeout == 8 && g_dt_timeout >= 28000 && g_dt_timeout < 500000;
  printf("timeout: bits=%d dt=%lldus ok=%s\n", g_bits_timeout, g_dt_timeout,
         ok1 ? "PASS" : "FAIL");
  int ok2 = g_bits_read == 1 && g_dt_read < 200000;
  printf("read-before-deadline: bits=%d dt=%lldus ok=%s\n", g_bits_read,
         g_dt_read, ok2 ? "PASS" : "FAIL");
  int ok3 = g_abandoned == 0;
  printf("no-leak: ok=%s (abandoned=%d)\n", ok3 ? "PASS" : "FAIL",
         g_abandoned);
  return (ok1 && ok2 && ok3) ? 0 : 1;
}
