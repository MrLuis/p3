/* loop_echo.c: the reference echo server, in the style of mill's loopsrv:
 * one event loop per thread, every socket registered once, read until
 * EAGAIN, answer on the same thread, events taken in batches of 256.
 *
 *   loop_echo PORT [threads=1]
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>

#include "poll.h"

static int g_listen;

static void *loop_main(void *arg) {
  (void)arg;
  int pfd = bp_create();
  bp_add_read(pfd, g_listen, NULL);
  bp_event evs[256];
  char buf[65536];
  for (;;) {
    int n = bp_wait(pfd, evs, 256, 1000);
    for (int i = 0; i < n; i++) {
      if (evs[i].data == NULL) {
        for (;;) {
          int fd = accept(g_listen, NULL, NULL);
          if (fd < 0)
            break;
          int one = 1;
          setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
          bp_nonblock(fd);
          bp_add_read(pfd, fd, (void *)(intptr_t)(fd + 1));
        }
        continue;
      }
      int fd = (int)(intptr_t)evs[i].data - 1;
      for (;;) {
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r > 0) {
          if (send(fd, buf, (size_t)r, 0) != r) {
            close(fd);
            break;
          }
          continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EINTR))
          break;
        close(fd);
        break;
      }
    }
  }
  return NULL;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: loop_echo PORT [threads]\n");
    return 2;
  }
  signal(SIGPIPE, SIG_IGN);
  int threads = argc > 2 ? atoi(argv[2]) : 1;
  g_listen = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)atoi(argv[1]));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(g_listen, (struct sockaddr *)&a, sizeof a) != 0 || listen(g_listen, 4096) != 0) {
    perror("bind/listen");
    return 1;
  }
  bp_nonblock(g_listen);
  pthread_t th;
  for (int i = 1; i < threads; i++)
    pthread_create(&th, NULL, loop_main, NULL);
  loop_main(NULL);
  return 0;
}
