/* pingpong.c: request/response load for the netasync benchmark.
 *
 *   pingpong PORT [conns=150] [seconds=5] [size=64] [threads=4]
 *
 * Opens `conns` keep-alive connections to 127.0.0.1:PORT, spread over
 * `threads` client threads. Every connection sends one `size`-byte message,
 * waits until all `size` bytes came back, and repeats — one message in
 * flight per connection, like an HTTP keep-alive client. After a 1 s
 * warm-up it counts round trips for `seconds` and prints
 *   rps=<round trips per second> p50=<us> p99=<us>
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>

#include "poll.h"

typedef struct {
  int fd;
  int got;
  int64_t sent_ns;
} Conn;

static int g_port, g_size, g_seconds;
static volatile int g_phase; /* 0 warm-up, 1 measuring, 2 stop */
static long long g_lat_buckets[64]; /* log2(us) histogram, under g_mu */
static long long g_count;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
  int nconns;
} ThreadArgs;

static int connect_one(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)g_port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
    perror("connect");
    exit(1);
  }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  bp_nonblock(fd);
  return fd;
}

static void send_msg(Conn *c, const char *msg) {
  c->got = 0;
  c->sent_ns = bp_now_ns();
  ssize_t n = send(c->fd, msg, (size_t)g_size, 0);
  if (n != g_size) {
    fprintf(stderr, "short send (%zd)\n", n);
    exit(1);
  }
}

static void *client_main(void *arg) {
  ThreadArgs *ta = arg;
  int pfd = bp_create();
  Conn *conns = calloc((size_t)ta->nconns, sizeof *conns);
  char *msg = malloc((size_t)g_size), *buf = malloc(65536);
  memset(msg, 'p', (size_t)g_size);
  long long buckets[64] = {0}, count = 0;
  for (int i = 0; i < ta->nconns; i++) {
    conns[i].fd = connect_one();
    bp_add_read(pfd, conns[i].fd, &conns[i]);
  }
  for (int i = 0; i < ta->nconns; i++)
    send_msg(&conns[i], msg);
  bp_event evs[256];
  while (g_phase < 2) {
    int n = bp_wait(pfd, evs, 256, 100);
    for (int i = 0; i < n; i++) {
      Conn *c = evs[i].data;
      ssize_t r = recv(c->fd, buf, 65536, 0);
      if (r <= 0) {
        if (r < 0 && (errno == EAGAIN || errno == EINTR))
          continue;
        fprintf(stderr, "server closed a connection\n");
        exit(1);
      }
      c->got += (int)r;
      if (c->got < g_size)
        continue;
      if (g_phase == 1) {
        int64_t us = (bp_now_ns() - c->sent_ns) / 1000;
        int b = 0;
        while ((1LL << b) < us && b < 63)
          b++;
        buckets[b]++;
        count++;
      }
      send_msg(c, msg);
    }
  }
  pthread_mutex_lock(&g_mu);
  for (int b = 0; b < 64; b++)
    g_lat_buckets[b] += buckets[b];
  g_count += count;
  pthread_mutex_unlock(&g_mu);
  for (int i = 0; i < ta->nconns; i++)
    close(conns[i].fd);
  return NULL;
}

static long long percentile(double p) {
  long long want = (long long)(g_count * p), seen = 0;
  for (int b = 0; b < 64; b++) {
    seen += g_lat_buckets[b];
    if (seen >= want && g_lat_buckets[b])
      return 1LL << b;
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: pingpong PORT [conns] [seconds] [size] [threads]\n");
    return 2;
  }
  g_port = atoi(argv[1]);
  int conns = argc > 2 ? atoi(argv[2]) : 150;
  g_seconds = argc > 3 ? atoi(argv[3]) : 5;
  g_size = argc > 4 ? atoi(argv[4]) : 64;
  int threads = argc > 5 ? atoi(argv[5]) : 4;
  if (threads > conns)
    threads = conns;
  pthread_t th[64];
  ThreadArgs ta[64];
  for (int i = 0; i < threads; i++) {
    ta[i].nconns = conns / threads + (i < conns % threads ? 1 : 0);
    pthread_create(&th[i], NULL, client_main, &ta[i]);
  }
  struct timespec warm = {1, 0}, run = {g_seconds, 0};
  nanosleep(&warm, NULL);
  g_phase = 1;
  int64_t t0 = bp_now_ns();
  nanosleep(&run, NULL);
  g_phase = 2;
  int64_t t1 = bp_now_ns();
  for (int i = 0; i < threads; i++)
    pthread_join(th[i], NULL);
  double secs = (double)(t1 - t0) / 1e9;
  printf("rps=%.0f p50<=%lldus p99<=%lldus\n", (double)g_count / secs,
         percentile(0.50), percentile(0.99));
  return 0;
}
