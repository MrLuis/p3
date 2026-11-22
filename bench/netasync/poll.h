/* poll.h: the smallest kqueue/epoll shim the netasync benchmarks share.
 * Level-triggered read interest, one registration per socket for its whole
 * life (the event-loop style of mill's loopsrv). */
#ifndef BENCH_POLL_H
#define BENCH_POLL_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/epoll.h>
#else
#include <sys/event.h>
#endif

typedef struct {
  void *data;
  int readable, writable, error;
} bp_event;

static inline int bp_create(void) {
#ifdef __linux__
  return epoll_create1(0);
#else
  return kqueue();
#endif
}

static inline void bp_add_read(int pfd, int fd, void *data) {
#ifdef __linux__
  struct epoll_event ev;
  memset(&ev, 0, sizeof ev);
  ev.events = EPOLLIN;
  ev.data.ptr = data;
  epoll_ctl(pfd, EPOLL_CTL_ADD, fd, &ev);
#else
  struct kevent k;
  EV_SET(&k, fd, EVFILT_READ, EV_ADD, 0, 0, data);
  kevent(pfd, &k, 1, NULL, 0, NULL);
#endif
}

/* returns the number of events (0 on timeout), filling out[] */
static inline int bp_wait(int pfd, bp_event *out, int max, int timeout_ms) {
#ifdef __linux__
  struct epoll_event evs[256];
  if (max > 256) max = 256;
  int n = epoll_wait(pfd, evs, max, timeout_ms);
  for (int i = 0; i < n; i++) {
    out[i].data = evs[i].data.ptr;
    out[i].readable = (evs[i].events & EPOLLIN) != 0;
    out[i].writable = (evs[i].events & EPOLLOUT) != 0;
    out[i].error = (evs[i].events & (EPOLLERR | EPOLLHUP)) != 0;
  }
  return n < 0 ? 0 : n;
#else
  struct kevent evs[256];
  struct timespec ts = {timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L};
  if (max > 256) max = 256;
  int n = kevent(pfd, NULL, 0, evs, max, &ts);
  for (int i = 0; i < n; i++) {
    out[i].data = evs[i].udata;
    out[i].readable = evs[i].filter == EVFILT_READ;
    out[i].writable = evs[i].filter == EVFILT_WRITE;
    out[i].error = (evs[i].flags & EV_ERROR) != 0;
  }
  return n < 0 ? 0 : n;
#endif
}

static inline void bp_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static inline int64_t bp_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#endif
