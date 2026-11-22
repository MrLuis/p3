/*
 * Tasks: async functions, await, structured concurrency (scopes), the
 * thread pool, cancellation tokens, timers, and the mutexes and wait queues
 * the unit `threading` builds channels on.
 *
 * A task runs on a stack of its own (a coroutine: ucontext), so the code of
 * an async function is ordinary code: `try` blocks, exceptions (setjmp and
 * longjmp, each task with its own chain of exception frames) and nested
 * calls work across `await`. A task belongs to the thread it was started
 * on (its home), which alone runs it: when it waits, its thread goes on
 * with other work, and when what it waits for is done it is queued on its
 * home again. Threads run their queued tasks in a loop: pool threads
 * forever, other threads (the main program's) while they wait for a task
 * (Wait, a scope's end, Sleep).
 *
 * One lock (__pas_async_mutex) protects all the shared state here: tasks'
 * states and waiters, queues, scopes, tokens, timers, wait queues.
 */
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700 /* ucontext */
#endif
#include "pascalrt.h"

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/epoll.h>
#else
#include <sys/event.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define PAS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PAS_ASAN 1
#endif
#endif
#ifdef PAS_ASAN
#include <sanitizer/common_interface_defs.h>
#endif

int _setjmp(jmp_buf env);
extern _Thread_local PasExcFrame *__pas_exc_top;

/* (Variables have external linkage: see utils/gen-runtime-ir.py.) */
pthread_mutex_t __pas_async_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&__pas_async_mutex)
#define UNLOCK() pthread_mutex_unlock(&__pas_async_mutex)

enum { TS_NEW, TS_RUNNING, TS_WAITING, TS_READY, TS_DONE, TS_FAILED };
enum { WAKE_NORMAL, WAKE_CANCEL };

typedef struct PasContext PasContext;
typedef struct PasScope PasScope;

/* Who waits: a task (suspended), or a thread outside any task (blocked on
 * its context's condition). */
typedef struct PasWaiter {
  struct PasWaiter *next;
  PasTask *task;
  PasContext *thread;
  int signaled; /* (set when woken) */
} PasWaiter;

struct PasToken {
  int64_t refs;
  int cancelled;
  struct PasToken *parent, *parent2; /* cancelled with either (counted) */
  PasWaiter *waiters;      /* tasks waiting while they observe it */
};

struct PasTask {
  int64_t refs;
  int state;
  int wake;      /* why it was woken: WAKE_* */
  int detached;
  const PasDynInfo *info; /* the result's type, or NULL */
  void (*entry)(PasTask *);
  void (*args_fini)(void *args);
  int64_t result_size;
  PasContext *home;
  ucontext_t ctx;
  void *stack;
  size_t stack_size;
  ucontext_t *resumer;          /* where it goes when it waits or ends */
  const void *resumer_bottom;   /* (the resumer's stack, for ASan) */
  size_t resumer_size;
  void *fake_stack;
  PasExcFrame *exc_top;         /* its exception frames, while switched out */
  void *exception;              /* TS_FAILED: the exception, until raised */
  const char *exc_where;        /* (where it was raised) */
  PasWaiter *waiters;           /* those waiting for it to end */
  PasTask *next_ready;
  PasScope *scope;              /* the scope it was spawned into */
  PasScope *scopes;             /* the innermost scope open in it */
  PasToken *token;              /* the token it observes (counted) */
  PasClosure on_error;          /* detached: its error handler */
  char data[];                  /* the result, then the arguments */
};

struct PasContext {
  pthread_cond_t cv;
  PasTask *head, *tail; /* ready */
  PasTask *current;     /* the task running on this thread, or NULL */
  PasTask *starting;    /* (handed to task_main) */
  PasScope *scopes;     /* scopes open outside any task */
  int is_pool;
};

struct PasScope {
  PasScope *outer;
  PasToken *token;       /* its own (a child of the one it was given) */
  PasToken *saved_token; /* the owner's token before it */
  PasTask **children;
  int64_t count, cap;
};

_Thread_local PasContext *__pas_async_ctx;
PasContext **__pas_async_pool;
int __pas_async_pool_size;
int __pas_async_pool_next;

extern void *const __pas_eoperationcancelled_vmt[7];
void __pas_raise_message(void *const *vmt, const char *text)
    __attribute__((noreturn));

/*===- Contexts and the ready queues ----------------------------------------===*/

static PasContext *new_context(int is_pool) {
  PasContext *c = calloc(1, sizeof *c);
  if (!c)
    __pas_runtime_error("out of memory for a thread's tasks");
  pthread_cond_init(&c->cv, NULL);
  c->is_pool = is_pool;
  return c;
}

static PasContext *context(void) {
  if (!__pas_async_ctx)
    __pas_async_ctx = new_context(0);
  return __pas_async_ctx;
}

/* Under the lock: t joins its home's ready queue. */
static void post(PasTask *t) {
  PasContext *c = t->home;
  t->next_ready = NULL;
  if (c->tail)
    c->tail->next_ready = t;
  else
    c->head = t;
  c->tail = t;
  pthread_cond_signal(&c->cv);
}

static PasTask *pop(PasContext *c) {
  PasTask *t = c->head;
  if (t) {
    c->head = t->next_ready;
    if (!c->head)
      c->tail = NULL;
  }
  return t;
}

/* Under the lock: w is woken (a task: queued, unless it already was). */
static void wake(PasWaiter *w, int why) {
  w->signaled = 1;
  if (w->task) {
    PasTask *t = w->task;
    if (t->state != TS_WAITING)
      return;
    t->state = TS_READY;
    t->wake = why;
    post(t);
  } else {
    pthread_cond_broadcast(&w->thread->cv);
  }
}

static void add_waiter(PasWaiter **list, PasWaiter *w) {
  w->next = *list;
  *list = w;
}

static void remove_waiter(PasWaiter **list, PasWaiter *w) {
  for (PasWaiter **p = list; *p; p = &(*p)->next)
    if (*p == w) {
      *p = w->next;
      return;
    }
}

/*===- Tokens ----------------------------------------------------------------===*/

void __pas_token_addref(PasToken *k) {
  if (k)
    __atomic_fetch_add(&k->refs, 1, __ATOMIC_RELAXED);
}

/* A new token, cancelled with parent and with parent2 (either may be nil). */
static PasToken *token_new2(PasToken *parent, PasToken *parent2) {
  PasToken *k = calloc(1, sizeof *k);
  if (!k)
    __pas_runtime_error("out of memory for a cancellation token");
  k->refs = 1;
  k->parent = parent;
  k->parent2 = parent2;
  __pas_token_addref(parent);
  __pas_token_addref(parent2);
  return k;
}

PasToken *__pas_token_new(PasToken *parent) { return token_new2(parent, NULL); }

void __pas_token_release(PasToken *k) {
  if (k && __atomic_sub_fetch(&k->refs, 1, __ATOMIC_ACQ_REL) == 0) {
    PasToken *p = k->parent, *p2 = k->parent2;
    free(k);
    __pas_token_release(p);
    __pas_token_release(p2);
  }
}

void __pas_token_assign(PasToken **dst, PasToken *src) {
  __pas_token_addref(src);
  PasToken *old = *dst;
  *dst = src;
  __pas_token_release(old);
}

void __pas_token_clear(PasToken **p) {
  PasToken *old = *p;
  *p = NULL;
  __pas_token_release(old);
}

static int cancelled_locked(PasToken *k) {
  return k && (k->cancelled || cancelled_locked(k->parent) ||
               cancelled_locked(k->parent2));
}

int32_t __pas_token_cancelled(PasToken *k) {
  LOCK();
  int c = cancelled_locked(k);
  UNLOCK();
  return c;
}

/* Under the lock. The tasks that wait observing k, or a token made from
 * it, are registered with k too (see observe). */
static void cancel_locked(PasToken *k) {
  if (!k || k->cancelled)
    return;
  k->cancelled = 1;
  for (PasWaiter *w = k->waiters; w; w = w->next)
    wake(w, WAKE_CANCEL);
}

void __pas_token_cancel(PasToken *k) {
  LOCK();
  cancel_locked(k);
  UNLOCK();
}

/* A task about to wait: registered with its token and the token's
 * ancestors, so that cancelling any wakes it. nodes: one per token. */
#define OBSERVED 16
typedef struct {
  PasWaiter nodes[OBSERVED];
  PasToken *tokens[OBSERVED];
  int n;
} Observation;

static void observe_token(PasTask *t, PasToken *k, Observation *o) {
  if (!k || o->n == OBSERVED)
    return;
  for (int i = 0; i < o->n; ++i)
    if (o->tokens[i] == k)
      return;
  memset(&o->nodes[o->n], 0, sizeof(PasWaiter));
  o->nodes[o->n].task = t;
  o->tokens[o->n] = k;
  add_waiter(&k->waiters, &o->nodes[o->n]);
  o->n++;
  observe_token(t, k->parent, o);
  observe_token(t, k->parent2, o);
}

static void observe(PasTask *t, Observation *o) {
  o->n = 0;
  observe_token(t, t->token, o);
}

static void unobserve(Observation *o) {
  for (int i = 0; i < o->n; ++i)
    remove_waiter(&o->tokens[i]->waiters, &o->nodes[i]);
}

static void raise_cancelled(void) __attribute__((noreturn));
static void raise_cancelled(void) {
  __pas_raise_message(__pas_eoperationcancelled_vmt, "Operation cancelled");
}

/*===- Switching -------------------------------------------------------------===*/

#define STACK_SIZE (1024 * 1024)
#define STACK_CACHE 16
void *__pas_async_stacks[STACK_CACHE];
int __pas_async_nstacks;

#ifdef __APPLE__
/* macOS malloc reserves a huge span of address space (MAGMA: observed
   0x1000000000..0x7000000000 on macOS 26) whose slabs it brings up by
   changing their protection as needed. An anonymous mmap(NULL) can land
   inside it, and the allocator's next slab then runs into the stack's
   guard page (SIGBUS in the middle of malloc). Task stacks go above it. */
#define PAS_MALLOC_RESERVE_LOW 0x1000000000ULL
#define PAS_MALLOC_RESERVE_HIGH 0x7000000000ULL
#define PAS_STACK_HINT_START 0xFC00000000ULL
static void *new_stack(void) {
  LOCK();
  void *s = __pas_async_nstacks ? __pas_async_stacks[--__pas_async_nstacks] : NULL;
  UNLOCK();
  if (s)
    return s;
  long page = sysconf(_SC_PAGESIZE);
  size_t len = STACK_SIZE + (size_t)page;
  char *m = mmap(NULL, len, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED)
    __pas_runtime_error("out of memory for a task's stack");
  if ((uintptr_t)m >= PAS_MALLOC_RESERVE_LOW &&
      (uintptr_t)m < PAS_MALLOC_RESERVE_HIGH) {
    munmap(m, len);
    uintptr_t hint = PAS_STACK_HINT_START;
    m = MAP_FAILED;
    for (int t = 0; t < 4096 && m == MAP_FAILED; t++, hint -= len) {
      m = mmap((void *)hint, len, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (m == MAP_FAILED)
        continue;
      if ((uintptr_t)m >= PAS_MALLOC_RESERVE_LOW &&
          (uintptr_t)m < PAS_MALLOC_RESERVE_HIGH) {
        munmap(m, len);
        m = MAP_FAILED;
      }
    }
    if (m == MAP_FAILED)
      __pas_runtime_error("out of memory for a task's stack");
  }
  mprotect(m, (size_t)page, PROT_NONE); /* a guard page below */
  return m + page;
}
#else
static void *new_stack(void) {
  LOCK();
  void *s = __pas_async_nstacks ? __pas_async_stacks[--__pas_async_nstacks] : NULL;
  UNLOCK();
  if (s)
    return s;
  long page = sysconf(_SC_PAGESIZE);
  char *m = mmap(NULL, STACK_SIZE + (size_t)page, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED)
    __pas_runtime_error("out of memory for a task's stack");
  mprotect(m, (size_t)page, PROT_NONE); /* a guard page below */
  return m + page;
}
#endif

static void free_stack(void *s) {
  LOCK();
  if (__pas_async_nstacks < STACK_CACHE) {
    __pas_async_stacks[__pas_async_nstacks++] = s;
    s = NULL;
  }
  UNLOCK();
  if (s) {
    long page = sysconf(_SC_PAGESIZE);
    munmap((char *)s - page, STACK_SIZE + (size_t)page);
  }
}

void __pas_task_release(PasTask *t);

/* From here into t until it waits or ends; from is where it comes back. */
static void switch_into(PasTask *t, ucontext_t *from) {
  PasContext *c = context();
  PasTask *prev = c->current;
  PasExcFrame *saved = __pas_exc_top;
  c->current = t;
  __pas_exc_top = t->exc_top;
  t->resumer = from;
#ifdef PAS_ASAN
  void *fake = NULL;
  __sanitizer_start_switch_fiber(&fake, t->stack, t->stack_size);
#endif
  swapcontext(from, &t->ctx);
#ifdef PAS_ASAN
  __sanitizer_finish_switch_fiber(fake, NULL, NULL);
#endif
  c->current = prev;
  __pas_exc_top = saved;
  if (t->state == TS_DONE || t->state == TS_FAILED) { /* it ended */
    free_stack(t->stack);
    t->stack = NULL;
    __pas_task_release(t); /* its own reference while it ran */
  }
}

/* In t: back to whoever resumed it; returns when it is resumed. */
static void suspend(PasTask *t) {
  t->exc_top = __pas_exc_top;
#ifdef PAS_ASAN
  __sanitizer_start_switch_fiber(&t->fake_stack, t->resumer_bottom, t->resumer_size);
#endif
  swapcontext(&t->ctx, t->resumer);
#ifdef PAS_ASAN
  __sanitizer_finish_switch_fiber(t->fake_stack, &t->resumer_bottom, &t->resumer_size);
#endif
}

static void finish(PasTask *t, void *exception);

static void task_main(void) {
  PasContext *c = context();
  PasTask *t = c->starting;
#ifdef PAS_ASAN
  __sanitizer_finish_switch_fiber(NULL, &t->resumer_bottom, &t->resumer_size);
#endif
  PasExcFrame f;
  memset(&f, 0, sizeof f);
  __pas_exc_push(&f);
  if (_setjmp(*(jmp_buf *)(void *)f.buf) == 0) {
    t->entry(t);
    __pas_exc_pop(&f);
    if (t->args_fini)
      t->args_fini(t->data + t->result_size);
    finish(t, NULL);
  } else {
    if (t->args_fini)
      t->args_fini(t->data + t->result_size);
    t->exc_where = f.where;
    finish(t, f.object);
  }
  t->exc_top = NULL;
#ifdef PAS_ASAN
  __sanitizer_start_switch_fiber(NULL, t->resumer_bottom, t->resumer_size);
#endif
  setcontext(t->resumer); /* (its stack is freed there) */
  __pas_runtime_error("a task resumed after it ended");
}

/* Starts t (a new task, here: its home's thread). */
static void start(PasTask *t, ucontext_t *from) {
  t->stack = new_stack();
  t->stack_size = STACK_SIZE;
  getcontext(&t->ctx);
  t->ctx.uc_stack.ss_sp = t->stack;
  t->ctx.uc_stack.ss_size = t->stack_size;
  t->ctx.uc_link = NULL;
  makecontext(&t->ctx, task_main, 0);
  LOCK();
  t->state = TS_RUNNING;
  UNLOCK();
  context()->starting = t;
  switch_into(t, from);
}

/* Runs t (ready or new) on this thread until it waits or ends. */
static void resume(PasTask *t) {
  ucontext_t here;
  if (t->state == TS_NEW) {
    start(t, &here);
    return;
  }
  LOCK();
  t->state = TS_RUNNING;
  UNLOCK();
  switch_into(t, &here);
}

/* Under the lock, on a thread outside any task: runs this thread's ready
 * tasks until done(arg), waiting when there are none (until deadline_ms,
 * a CLOCK_MONOTONIC time in ms, if not 0). */
static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void loop_until(PasContext *c, int (*done)(void *), void *arg,
                       int64_t deadline_ms) {
  while (!done(arg)) {
    PasTask *t = pop(c);
    if (t) {
      UNLOCK();
      resume(t);
      LOCK();
      continue;
    }
    if (deadline_ms) {
      int64_t left = deadline_ms - now_ms();
      if (left <= 0)
        return;
      /* (pthread_cond_timedwait takes CLOCK_REALTIME) */
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += left / 1000;
      ts.tv_nsec += (left % 1000) * 1000000;
      if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
      }
      pthread_cond_timedwait(&c->cv, &__pas_async_mutex, &ts);
    } else {
      pthread_cond_wait(&c->cv, &__pas_async_mutex);
    }
  }
}

static int never(void *arg) {
  (void)arg;
  return 0;
}

static void *pool_main(void *arg) {
  PasContext *c = arg;
  __pas_async_ctx = c;
  LOCK();
  loop_until(c, never, NULL, 0);
  UNLOCK();
  return NULL;
}

/* Before a thread is created: reference counts become atomic from now on
   (pascalrt.h). Set while this is the only thread, so a plain store does;
   pthread_create publishes it to the new thread. */
void __pas_start_threads(void) {
  __pas_threads_started = 1;
}

/* Under the lock: the pool, started when first needed. */
static PasContext *pool_context(void) {
  if (!__pas_async_pool) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    const char *env = getenv("P3_POOL_THREADS");
    if (env && atoi(env) > 0)
      n = atoi(env);
    if (n < 2)
      n = 2;
    if (n > 64)
      n = 64;
    __pas_async_pool = calloc((size_t)n, sizeof(PasContext *));
    for (long i = 0; i < n; ++i) {
      __pas_async_pool[i] = new_context(1);
      pthread_t th;
      pthread_attr_t a;
      pthread_attr_init(&a);
      pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
      __pas_start_threads();
      if (pthread_create(&th, &a, pool_main, __pas_async_pool[i]) != 0)
        __pas_runtime_error("cannot start the thread pool");
      pthread_attr_destroy(&a);
    }
    __pas_async_pool_size = (int)n;
  }
  PasContext *c = __pas_async_pool[__pas_async_pool_next];
  __pas_async_pool_next = (__pas_async_pool_next + 1) % __pas_async_pool_size;
  return c;
}

/*===- Tasks -----------------------------------------------------------------===*/

static int64_t round16(int64_t n) { return (n + 15) & ~(int64_t)15; }

PasTask *__pas_task_new(const PasDynInfo *info, int64_t args_size,
                        void (*entry)(PasTask *), void (*args_fini)(void *)) {
  int64_t rs = info ? round16(info->elem_size) : 0;
  PasTask *t = calloc(1, sizeof(PasTask) + (size_t)rs + (size_t)args_size);
  if (!t)
    __pas_runtime_error("out of memory for a task");
  t->refs = 1;
  t->info = info;
  t->entry = entry;
  t->args_fini = args_fini;
  t->result_size = rs;
  return t;
}

void *__pas_task_result(PasTask *t) { return t->data; }
void *__pas_task_args(PasTask *t) { return t->data + t->result_size; }

void __pas_task_addref(PasTask *t) {
  if (t)
    __atomic_fetch_add(&t->refs, 1, __ATOMIC_RELAXED);
}

void __pas_task_release(PasTask *t) {
  if (!t || __atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL) != 0)
    return;
  if (t->state == TS_DONE && t->info && t->info->fini)
    t->info->fini(t->data);
  if (t->exception)
    __pas_free_object(t->exception);
  __pas_token_release(t->token);
  free(t);
}

void __pas_task_assign(PasTask **dst, PasTask *src) {
  __pas_task_addref(src);
  PasTask *old = *dst;
  *dst = src;
  __pas_task_release(old);
}

void __pas_task_clear(PasTask **p) {
  PasTask *old = *p;
  *p = NULL;
  __pas_task_release(old);
}

/* The token the current code observes: its task's, or none. */
static PasToken *current_token(void) {
  PasTask *t = context()->current;
  return t ? t->token : NULL;
}

static PasScope **current_scopes(void) {
  PasContext *c = context();
  return c->current ? &c->current->scopes : &c->scopes;
}

/* An async routine called: t runs here at once, until it waits or ends,
 * observing the caller's token. */
void __pas_task_start(PasTask *t) {
  PasContext *c = context();
  t->home = c;
  t->token = current_token();
  __pas_token_addref(t->token);
  __pas_task_addref(t); /* its own, while it runs */
  ucontext_t here;
  start(t, &here);
}

/* Queues new task t on a pool thread. */
static void run_on_pool(PasTask *t) {
  __pas_task_addref(t); /* its own, while it runs */
  LOCK();
  t->home = pool_context();
  t->state = TS_NEW;
  post(t);
  UNLOCK();
}

/* spawn: t runs on the pool, a child of the innermost scope open here
 * (observing its token). */
void __pas_task_spawn(PasTask *t) {
  PasScope *s = *current_scopes();
  if (s) {
    LOCK();
    if (s->count == s->cap) {
      s->cap = s->cap ? 2 * s->cap : 8;
      s->children = realloc(s->children, (size_t)s->cap * sizeof(PasTask *));
      if (!s->children)
        __pas_runtime_error("out of memory in a scope");
    }
    s->children[s->count++] = t;
    __pas_task_addref(t);
    t->scope = s;
    t->token = s->token;
    __pas_token_addref(t->token);
    UNLOCK();
  }
  run_on_pool(t);
}

/* Pool.Run: t runs on the pool, observing the caller's token. */
void __pas_task_run_pool(PasTask *t) {
  t->token = current_token();
  __pas_token_addref(t->token);
  run_on_pool(t);
}

static void call_error_handler(PasTask *t, void *exception);

/* In t, its code done: the result is in place, or the exception. */
static void finish(PasTask *t, void *exception) {
  LOCK();
  t->state = exception ? TS_FAILED : TS_DONE;
  t->exception = exception;
  for (PasWaiter *w = t->waiters; w; w = w->next)
    wake(w, WAKE_NORMAL);
  t->waiters = NULL;
  if (exception && t->scope)
    cancel_locked(t->scope->token); /* the others stop */
  int handle = exception && t->detached && t->on_error.fn;
  if (handle)
    t->exception = NULL;
  UNLOCK();
  if (handle)
    call_error_handler(t, exception);
}

static int task_ended(void *arg) {
  PasTask *t = arg;
  return t->state == TS_DONE || t->state == TS_FAILED;
}

/* Under the lock: until t ends. In a task: it waits (cancelled if its
 * token is, unless ignore_cancel); outside: this thread runs its tasks. */
static void wait_for(PasTask *t, int ignore_cancel) {
  PasContext *c = context();
  PasTask *me = c->current;
  if (task_ended(t))
    return;
  if (!me) {
    PasWaiter w = {NULL, NULL, c, 0};
    add_waiter(&t->waiters, &w);
    loop_until(c, task_ended, t, 0);
    remove_waiter(&t->waiters, &w);
    return;
  }
  if (me == t)
    __pas_runtime_error("a task waits for itself");
  if (!ignore_cancel && cancelled_locked(me->token)) {
    UNLOCK();
    raise_cancelled();
  }
  PasWaiter w = {NULL, me, NULL, 0};
  add_waiter(&t->waiters, &w);
  Observation o;
  if (!ignore_cancel)
    observe(me, &o);
  me->state = TS_WAITING;
  me->wake = WAKE_NORMAL;
  UNLOCK();
  suspend(me);
  LOCK();
  me->state = TS_RUNNING;
  if (!ignore_cancel)
    unobserve(&o);
  remove_waiter(&t->waiters, &w);
  if (me->wake == WAKE_CANCEL && !task_ended(t)) {
    UNLOCK();
    raise_cancelled();
  }
  if (!task_ended(t)) /* (woken otherwise: wait again) */
    wait_for(t, ignore_cancel);
}

/* Raises t's exception (once; then a new one that says so). */
static void raise_failure(PasTask *t) __attribute__((noreturn));
static void raise_failure(PasTask *t) {
  LOCK();
  void *e = t->exception;
  t->exception = NULL;
  UNLOCK();
  if (e)
    __pas_raise(e, t->exc_where ? t->exc_where : "");
  __pas_raise_message(__pas_exception_vmt,
                      "the task failed; its exception was raised by an earlier await");
}

/* await t / Wait(t): after it, the result is at __pas_task_result(t). */
void __pas_task_await(PasTask *t) {
  if (!t)
    __pas_runtime_error("await of a nil task");
  LOCK();
  wait_for(t, 0);
  int failed = t->state == TS_FAILED;
  UNLOCK();
  if (failed)
    raise_failure(t);
}

int32_t __pas_task_done(PasTask *t) {
  LOCK();
  int d = t && task_ended(t);
  UNLOCK();
  return d;
}

/* detached: t's failure goes to handler (a procedure(E: TObject) value),
 * or nowhere. */
void __pas_task_detach(PasTask *t, void *handler_fn, void *handler_link) {
  LOCK();
  t->detached = 1;
  t->on_error.fn = handler_fn;
  t->on_error.frame = handler_link;
  void *e = NULL;
  if (t->state == TS_FAILED && handler_fn) {
    e = t->exception;
    t->exception = NULL;
  }
  UNLOCK();
  if (e)
    call_error_handler(t, e);
}

static void call_error_handler(PasTask *t, void *exception) {
  void (*fn)(void *, void *) = (void (*)(void *, void *))t->on_error.fn;
  PasExcFrame f;
  memset(&f, 0, sizeof f);
  __pas_exc_push(&f);
  if (_setjmp(*(jmp_buf *)(void *)f.buf) == 0) {
    fn(t->on_error.frame, exception);
    __pas_exc_pop(&f);
  } else if (f.object) { /* (an exception from the handler: dropped) */
    __pas_free_object(f.object);
  }
  __pas_free_object(exception);
}

/*===- Scopes ----------------------------------------------------------------===*/

/* scope [token] ... end: its children run observing a token of its own,
 * cancelled when one of them fails, when the token given is, or when the
 * block itself raises. */
void *__pas_scope_enter(PasToken *given) {
  PasScope *s = calloc(1, sizeof *s);
  if (!s)
    __pas_runtime_error("out of memory for a scope");
  PasContext *c = context();
  /* cancelled with the token given and with what the code observes */
  PasToken *outer = c->current ? c->current->token : NULL;
  s->token = token_new2(given, outer != given ? outer : NULL);
  PasScope **list = current_scopes();
  s->outer = *list;
  *list = s;
  if (c->current) { /* the block observes the scope's token */
    s->saved_token = c->current->token;
    c->current->token = s->token;
    __pas_token_addref(s->token);
  }
  return s;
}

/* The end of scope s (raised: the block is leaving with an exception, which
 * its caller raises again): waits for every child; raises the first
 * child's exception if none. */
void __pas_scope_leave(void *scope, int32_t raised) {
  PasScope *s = scope;
  PasContext *c = context();
  if (c->current) { /* the owner observes what it did before */
    PasToken *k = c->current->token;
    c->current->token = s->saved_token;
    __pas_token_release(k);
  }
  PasScope **list = current_scopes();
  *list = s->outer;
  LOCK();
  if (raised)
    cancel_locked(s->token);
  for (int64_t i = 0; i < s->count; ++i)
    wait_for(s->children[i], 1);
  void *first = NULL;
  for (int64_t i = 0; i < s->count; ++i) {
    PasTask *t = s->children[i];
    if (t->state == TS_FAILED && t->exception) {
      void *e = t->exception;
      t->exception = NULL;
      int cancel = __pas_class_is(e, __pas_eoperationcancelled_vmt);
      /* a child's own failure wins over the cancellations it caused */
      if (!first || (__pas_class_is(first, __pas_eoperationcancelled_vmt) && !cancel)) {
        if (first)
          __pas_free_object(first);
        first = e;
      } else {
        __pas_free_object(e);
      }
    }
    t->scope = NULL;
  }
  UNLOCK();
  for (int64_t i = 0; i < s->count; ++i)
    __pas_task_release(s->children[i]);
  free(s->children);
  __pas_token_release(s->token);
  free(s);
  if (first && !raised)
    __pas_raise(first, "");
  if (first)
    __pas_free_object(first);
}

/*===- Timers ----------------------------------------------------------------===*/

typedef struct PasTimer {
  struct PasTimer *next;
  int64_t at;      /* CLOCK_MONOTONIC ms */
  PasTask *task;   /* to wake (Sleep), or */
  PasToken *token; /* to cancel (WithTimeout; counted) */
  int fired;
} PasTimer;

PasTimer *__pas_async_timers;
pthread_cond_t __pas_async_timer_cv = PTHREAD_COND_INITIALIZER;
int __pas_async_timer_started;

static void *timer_main(void *arg) {
  (void)arg;
  LOCK();
  for (;;) {
    int64_t now = now_ms();
    while (__pas_async_timers && __pas_async_timers->at <= now) {
      PasTimer *m = __pas_async_timers;
      __pas_async_timers = m->next;
      m->fired = 1;
      if (m->task) {
        PasWaiter w = {NULL, m->task, NULL, 0};
        wake(&w, WAKE_NORMAL);
      } else {
        cancel_locked(m->token);
        PasToken *k = m->token;
        free(m);
        UNLOCK();
        __pas_token_release(k);
        LOCK();
      }
    }
    if (!__pas_async_timers) {
      pthread_cond_wait(&__pas_async_timer_cv, &__pas_async_mutex);
    } else {
      int64_t left = __pas_async_timers->at - now_ms();
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += left / 1000;
      ts.tv_nsec += (left % 1000) * 1000000;
      if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
      }
      pthread_cond_timedwait(&__pas_async_timer_cv, &__pas_async_mutex, &ts);
    }
  }
  return NULL;
}

/* Under the lock. */
static void add_timer(PasTimer *m) {
  if (!__pas_async_timer_started) {
    pthread_t th;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    __pas_start_threads();
    if (pthread_create(&th, &a, timer_main, NULL) != 0)
      __pas_runtime_error("cannot start the timer thread");
    pthread_attr_destroy(&a);
    __pas_async_timer_started = 1;
  }
  PasTimer **p = &__pas_async_timers;
  while (*p && (*p)->at <= m->at)
    p = &(*p)->next;
  m->next = *p;
  *p = m;
  pthread_cond_signal(&__pas_async_timer_cv);
}

static void remove_timer(PasTimer *m) {
  for (PasTimer **p = &__pas_async_timers; *p; p = &(*p)->next)
    if (*p == m) {
      *p = m->next;
      return;
    }
}

/* A token cancelled after ms milliseconds, and with parent. */
PasToken *__pas_token_with_timeout(PasToken *parent, int32_t ms) {
  PasToken *k = __pas_token_new(parent);
  PasTimer *m = calloc(1, sizeof *m);
  if (!m)
    __pas_runtime_error("out of memory for a timer");
  m->at = now_ms() + (ms > 0 ? ms : 0);
  m->token = k;
  __pas_token_addref(k);
  LOCK();
  add_timer(m);
  UNLOCK();
  return k;
}

static int never_done(void *arg) {
  (void)arg;
  return 0;
}

/* Sleep(ms): a task waits (cancellable); a thread runs its tasks meanwhile. */
void __pas_task_sleep(int32_t ms) {
  PasContext *c = context();
  PasTask *me = c->current;
  int64_t at = now_ms() + (ms > 0 ? ms : 0);
  LOCK();
  if (!me) {
    loop_until(c, never_done, NULL, at);
    UNLOCK();
    return;
  }
  if (cancelled_locked(me->token)) {
    UNLOCK();
    raise_cancelled();
  }
  PasTimer m;
  memset(&m, 0, sizeof m);
  m.at = at;
  m.task = me;
  add_timer(&m);
  Observation o;
  observe(me, &o);
  me->state = TS_WAITING;
  me->wake = WAKE_NORMAL;
  UNLOCK();
  suspend(me);
  LOCK();
  me->state = TS_RUNNING;
  unobserve(&o);
  if (!m.fired)
    remove_timer(&m);
  int cancelled = me->wake == WAKE_CANCEL;
  UNLOCK();
  if (cancelled)
    raise_cancelled();
}

/* *dst := a new token, with parent (and cancelled after ms if ms >= 0);
 * the reference moves into *dst (for unit threading). */
void __pas_token_make(PasToken *parent, int32_t ms, PasToken **dst) {
  PasToken *k = ms >= 0 ? __pas_token_with_timeout(parent, ms) : __pas_token_new(parent);
  PasToken *old = *dst;
  *dst = k;
  __pas_token_release(old);
}

/* Cancellation checked by hand (TCancelToken.ThrowIfCancelled). */
void __pas_token_check(PasToken *k) {
  if (__pas_token_cancelled(k))
    raise_cancelled();
}

/*===- Waiting on descriptors (unit netasync) ------------------------------===*/

/* A task parks until a descriptor is readable or writable (__pas_task_wait_fd)
 * or until an async read/send completes (__pas_task_recv/__pas_task_send).
 * One small poller thread holds the kernel interest and hands completions to
 * the tasks. Backends, chosen at runtime: io_uring (Linux, raw syscalls — no
 * liburing dependency), epoll (Linux fallback for kernels and sandboxes
 * without io_uring), and kqueue (macOS). On the io_uring backend a wait
 * submits io_uring ops (RECV/SEND/POLL_ADD) and is woken by its completion;
 * on the poll backends the syscall happens after the wake, in the task.
 *
 * A wait record belongs to its wait: the park helper frees it on return when
 * no completion can follow (poll backends; an io_uring completion reaped
 * before the task resumes). A cancelled io_uring wait abandons its record on
 * purpose — a completion may follow the task's rise, or never come — and
 * leaks it (bounded, ~72 bytes per cancellation); the record is marked freed
 * at the cancellation, and every completion reader skips freed records
 * without touching them again.
 *
 * Several records may wait on one descriptor at once (tasks of one or
 * several threads: a shared listener's acceptors, a database fd watched by
 * a driver task here and by the pool there). The poll backends arm each
 * record on its own dup of the fd, so every record is addressable on its
 * own and one record's retire cannot take the kernel interest away from
 * its still-parked peers; the readiness of the underlying file description
 * wakes each of the records parked on it, and the extra ones simply find
 * the transient EAGAIN their next syscall would produce. On io_uring every
 * wait is already its own op keyed by user_data, so nothing is shared.
 *
 * The wait reports what fired, as a bitmask it returns: 1 = readable,
 * 2 = writable, 4 = the descriptor is in an error state (POLLERR/POLLHUP
 * on the poll backends, a failed poll completion on io_uring). With the
 * dup-per-record registrations each record's report carries the event of
 * its own registration, not of a shared one. Everything here is covered by
 * the same lock as the rest of the file. */

enum {
  FD_BACKEND_KQUEUE = 0,
  FD_BACKEND_EPOLL = 1,
  FD_BACKEND_URING = 2,
  FD_OP_RECV = 1,
  FD_OP_SEND = 2,
  FD_OP_POLLREAD = 3,
  FD_OP_POLLWRITE = 4
};

/* The kernel's __kernel_timespec for IORING_OP_TIMEOUT (16 bytes); the
 * wait record embeds one, so it is declared for all backends. */
struct pas_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

typedef struct PasFdWait {
  int fd;
  int arm_fd;        /* what the poll backends registered (a dup of fd); -1 on io_uring and before arming */
  int write;         /* 1: waiting to write */
  int op;            /* FD_OP_* (io_uring) */
  int armed;         /* kernel interest in place (poll backends) */
  int state;         /* io_uring: 0 waiting, 1 cancelled, 2 completed;
                        poll backends: 1 retired */
  int freed;         /* done for good: completions skip it, no free */
  int pending;       /* io_uring: completions still in flight (2 for a
                        record armed with a linked timeout, 1 otherwise) */
  int seen;          /* io_uring: its first completion is processed */
  int left;          /* io_uring: the task resumed and consumed the result;
                        the record is freed by the reaper that takes
                        pending to 0 (the task may be gone by then) */
  int has_deadline;  /* 1: armed with a linked timeout (io_uring) or on
                        the poller's deadline list (poll backends) */
  int deadline_ms;   /* io_uring: the linked timeout's value */
  int64_t deadline_us; /* poll backends: when it fires (monotonic us) */
  struct pas_timespec ts; /* io_uring: the linked timeout's timespec */
  int64_t res;       /* io_uring: completion result (POLL_ADD: revents,
                        or negative on failure); poll backends: the
                        bits the poller mapped at the wake (1/2/4, or
                        8 = the deadline fired first) */
  void *buf;         /* io_uring: recv/send buffer */
  int32_t len;
  PasWaiter w;
  struct PasFdWait *next;
  struct PasFdWait *dnext; /* the poller's deadline list */
} PasFdWait;

#ifdef __linux__
/* Raw io_uring: constants and ring/SQE/CQE structures defined here, so the
 * runtime needs neither liburing nor linux/io_uring.h. The opcodes are
 * io_uring.h's enum numbers (SEND 26, RECV 27 — the older 9/10 are
 * SENDMSG/RECVMSG, which take a struct msghdr, not a plain buffer). POLL_ADD
 * reads its events from the SQE's poll_events/msg_flags union, not from len. */
#ifndef IORING_OP_POLL_ADD
#define IORING_OP_POLL_ADD 6
#define IORING_OP_POLL_REMOVE 7
#define IORING_OP_TIMEOUT 11
#define IORING_OP_SEND 26
#define IORING_OP_RECV 27
#endif
#ifndef IOSQE_IO_LINK
#define IOSQE_IO_LINK (1U << 3)
#endif
#ifndef ETIME
#define ETIME 62
#endif
#ifndef ECANCELLED
#define ECANCELLED 125
#endif
#ifndef IORING_ENTER_GETEVENTS
#define IORING_ENTER_GETEVENTS 1
#endif
#ifndef IORING_OFF_SQ_RING
#define IORING_OFF_SQ_RING 0ULL
#define IORING_OFF_CQ_RING 0x8000000ULL
#define IORING_OFF_SQES 0x10000000ULL
#endif
#ifndef SYS_io_uring_setup
#define SYS_io_uring_setup 425
#define SYS_io_uring_enter 426
#endif
#define PAS_URING_ENTRIES 16384

struct pas_uring_sqe {
  uint8_t opcode;
  uint8_t flags;
  uint16_t ioprio;
  int32_t fd;
  uint64_t off;
  uint64_t addr;
  uint32_t len;
  uint32_t msg_flags;
  uint64_t user_data;
  uint16_t buf_index;
  uint16_t personality;
  int32_t splice_fd_in;
  uint64_t pad2[2];
};

struct pas_uring_cqe {
  uint64_t user_data;
  int32_t res;
  uint32_t flags;
};

struct pas_uring_sq_off {
  uint32_t head, tail, ring_mask, ring_entries, flags, dropped, array;
  uint32_t resv[3];
};

struct pas_uring_cq_off {
  uint32_t head, tail, ring_mask, ring_entries, overflow, cqes;
  uint32_t resv[3];   /* (the kernel's struct is 120 bytes total) */
};

struct pas_uring_params {
  uint32_t sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle,
           features, wq_fd;
  uint32_t resv[3];
  struct pas_uring_sq_off sq_off;
  struct pas_uring_cq_off cq_off;
};
#endif

static void fd_poller_start(void);
static void fd_retire(PasFdWait *rec);
int32_t __pas_task_wait_fd2(int32_t fd, int32_t want_write, int32_t deadline_ms);
static PasFdWait *fd_waits;       /* records under the lock */
static PasFdWait *fd_deadlines;   /* poll-backend records with a deadline */
static int64_t fd_waits_abandoned; /* records a cancelled io_uring wait left behind */
static int fd_backend = FD_BACKEND_KQUEUE;
static int fd_poll_fd = -1;
static int fd_poll_started;
static pthread_t fd_poller_thread;

#ifdef __linux__
static void *ur_sq_ring, *ur_cq_ring;
static unsigned *ur_sq_head, *ur_sq_tail, *ur_sq_mask, *ur_sq_array;
static unsigned *ur_cq_head, *ur_cq_tail, *ur_cq_mask;
static struct pas_uring_cqe *ur_cqes;
static struct pas_uring_sqe *ur_sqes;
#endif

static void fd_unlink(PasFdWait *rec) {
  PasFdWait **p = &fd_waits;
  while (*p && *p != rec)
    p = &(*p)->next;
  if (*p)
    *p = rec->next;
}

/* The bits __pas_task_wait_fd returns: 1 = readable, 2 = writable,
 * 4 = failed. */

#ifdef __linux__
static int64_t fd_bits_epoll(uint32_t events) {
  int64_t bits = 0;
  if (events & EPOLLIN)
    bits |= 1;
  if (events & EPOLLOUT)
    bits |= 2;
  if (events & (EPOLLERR | EPOLLHUP))
    bits |= 4;
  return bits;
}
#else
static int64_t fd_bits_kqueue(const struct kevent *ev) {
  int64_t bits = (ev->filter == EVFILT_WRITE) ? 2 : 1;
  if ((ev->flags & EV_EOF) && ev->filter == EVFILT_WRITE)
    bits |= 4;   /* the peer stopped receiving: sends end in EPIPE */
  /* an EV_EOF on EVFILT_READ is a normal read side at EOF */
  return bits;
}
#endif

static int64_t fd_now_us(void) {
  struct timespec ts;
#ifdef __linux__
  clock_gettime(CLOCK_MONOTONIC, &ts);
#else
  clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
  return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000;
}

/* Under the lock: the record is done for its task (poll backends). The
 * drop releases the record's own registration (see fd_arm); the caller's
 * fd itself is never closed here. */
static void fd_retire(PasFdWait *rec) {
  if (rec->state != 0)
    return;
  rec->state = 1;
  if (rec->has_deadline) {
    PasFdWait **p = &fd_deadlines;
    while (*p && *p != rec)
      p = &(*p)->dnext;
    if (*p)
      *p = rec->dnext;
    rec->dnext = NULL;
    rec->has_deadline = 0;
  }
  if (rec->armed) {
    rec->armed = 0;
    int afd = rec->arm_fd;
    rec->arm_fd = -1;
    if (afd >= 0) {
#ifdef __linux__
      epoll_ctl(fd_poll_fd, EPOLL_CTL_DEL, afd, NULL);
#else
      struct kevent kn;
      memset(&kn, 0, sizeof kn);
      kn.ident = (uintptr_t)afd;
      kn.filter = rec->write ? EVFILT_WRITE : EVFILT_READ;
      kn.flags = EV_DELETE;
      kevent(fd_poll_fd, &kn, 1, NULL, 0, NULL);
#endif
      if (afd != rec->fd)
        close(afd);
    }
  }
}

/* Under the lock: records whose deadline passed get their NW_TIMEOUT
 * wake. The deadline list belongs to the poll backends (io_uring pairs
 * the wait with a linked TIMEOUT op instead). */
static void fd_fire_deadlines(void) {
  int64_t now = fd_now_us();
  PasFdWait **p = &fd_deadlines;
  while (*p) {
    PasFdWait *rec = *p;
    if (rec->deadline_us > now) {
      p = &rec->dnext;
      continue;
    }
    *p = rec->dnext;
    rec->dnext = NULL;
    rec->has_deadline = 0;
    if (rec->state != 0)
      continue;
    rec->res = 8;   /* NW_TIMEOUT: the deadline won the race */
    wake(&rec->w, WAKE_NORMAL);
    fd_retire(rec);
  }
}

/* Under the lock: how long the poller may block. The 10 ms cap bounds how
 * long a deadline added *while* it sleeps has to wait — without it, a
 * record parked with a short deadline could sit out a full 1 s block
 * (a wake pipe would avoid the quantum; the 100 polls/s idle cost is
 * cheaper than any new machinery). */
static int fd_poll_wait_ms(void) {
  PasFdWait *p = fd_deadlines;
  int64_t best = 0;
  while (p) {
    if (best == 0 || p->deadline_us < best)
      best = p->deadline_us;
    p = p->dnext;
  }
  if (best == 0)
    return 10;
  int64_t ms = (best - fd_now_us() + 999) / 1000;
  if (ms < 1)
    return 1;
  if (ms > 10)
    return 10;
  return (int)ms;
}

#ifdef __linux__
/* Under the lock: process one CQE of the ring (defined below the submit
 * helper; declared here because the full-ring rescue inside it reaps). */
static void uring_reap_cqe(struct pas_uring_cqe *cqe);
/* Fills and publishes the SQE of rec, then submits (under the lock; the
 * caller holds it). The slot stays occupied until a completion is reaped. */
static void uring_submit_rec(PasFdWait *rec) {
  struct pas_uring_sqe *sqe;
  unsigned tail, head, idx;
  int tries = 0;
  for (;;) {
    head = __atomic_load_n(ur_cq_head, __ATOMIC_ACQUIRE);
    tail = __atomic_load_n(ur_sq_tail, __ATOMIC_RELAXED);
    /* a record with a deadline publishes two linked SQEs */
    unsigned need = rec->has_deadline ? 2 : 1;
    if (tail - head + need <= *ur_sq_mask)
      break;
    /* full: reap what completed already; drop the lock around the yield so
     * the poller can go on reaping for the other threads */
    unsigned h2 = head;
    unsigned ct = __atomic_load_n(ur_cq_tail, __ATOMIC_ACQUIRE);
    while (h2 != ct) {
      uring_reap_cqe(&ur_cqes[h2 & *ur_sq_mask]);
      h2++;
    }
    __atomic_store_n(ur_cq_head, h2, __ATOMIC_RELEASE);
    if (tail - h2 + need <= *ur_sq_mask)
      break;
    if (tries++ > 1000)
      __pas_runtime_error("netasync: too many concurrent socket waits");
    UNLOCK();
    sched_yield();
    LOCK();
  }
  idx = tail & *ur_sq_mask;
  sqe = &ur_sqes[idx];
  memset(sqe, 0, sizeof *sqe);
  sqe->user_data = (uint64_t)(uintptr_t)rec;
  switch (rec->op) {
    case FD_OP_RECV:
      sqe->opcode = IORING_OP_RECV;
      sqe->fd = rec->fd;
      sqe->addr = (uint64_t)(uintptr_t)rec->buf;
      sqe->len = (uint32_t)rec->len;
      break;
    case FD_OP_SEND:
      sqe->opcode = IORING_OP_SEND;
      sqe->fd = rec->fd;
      sqe->addr = (uint64_t)(uintptr_t)rec->buf;
      sqe->len = (uint32_t)rec->len;
      sqe->msg_flags = MSG_NOSIGNAL;
      break;
    case FD_OP_POLLREAD:
      sqe->opcode = IORING_OP_POLL_ADD;
      sqe->fd = rec->fd;
      sqe->msg_flags = POLLIN;   /* (the poll_events field) */
      break;
    default:
      sqe->opcode = IORING_OP_POLL_ADD;
      sqe->fd = rec->fd;
      sqe->msg_flags = POLLOUT;
      break;
  }
  __atomic_store_n(ur_sq_tail, tail + 1, __ATOMIC_RELEASE);
  if (rec->has_deadline) {
    /* the wait's own op is left unlinked: an independent TIMEOUT is
     * submitted next to it, and either completes first (the pair's
     * pending/left logic in the reaper handles the remnant; no link) */
    struct pas_uring_sqe *sqe2 = &ur_sqes[(tail + 1) & *ur_sq_mask];
    memset(sqe2, 0, sizeof *sqe2);
    sqe2->user_data = (uint64_t)(uintptr_t)rec;
    sqe2->opcode = IORING_OP_TIMEOUT;
    rec->ts.tv_sec = (int64_t)rec->deadline_ms / 1000;
    rec->ts.tv_nsec = (int64_t)(rec->deadline_ms % 1000) * 1000000;
    sqe2->addr = (uint64_t)(uintptr_t)&rec->ts;
    sqe2->len = 1;
    __atomic_store_n(ur_sq_tail, tail + 2, __ATOMIC_RELEASE);
    int r = syscall(SYS_io_uring_enter, fd_poll_fd, 2, 0, 0, NULL, 0);
    (void)r;
    return;
  }
  int r = syscall(SYS_io_uring_enter, fd_poll_fd, 1, 0, 0, NULL, 0);
  (void)r;
}

/* A wait cancelled on the io_uring backend: for a poll op, ask the kernel
 * to stop it (best effort — the abandoned record never needs its
 * completion); recv/send complete on their own. */
static void uring_cancel_rec(PasFdWait *rec) {
  unsigned tail, head;
  if (rec->op != FD_OP_POLLREAD && rec->op != FD_OP_POLLWRITE)
    return;
  head = __atomic_load_n(ur_cq_head, __ATOMIC_ACQUIRE);
  tail = __atomic_load_n(ur_sq_tail, __ATOMIC_RELAXED);
  if (tail - head >= *ur_sq_mask)
    return;                    /* full: the poll completes on its own */
  struct pas_uring_sqe *sqe = &ur_sqes[tail & *ur_sq_mask];
  memset(sqe, 0, sizeof *sqe);
  sqe->opcode = IORING_OP_POLL_REMOVE;
  sqe->addr = (uint64_t)(uintptr_t)rec;
  __atomic_store_n(ur_sq_tail, tail + 1, __ATOMIC_RELEASE);
  int r = syscall(SYS_io_uring_enter, fd_poll_fd, 1, 0, 0, NULL, 0);
  (void)r;
}

/* Under the lock: one CQE reaped. The first completion of a record stores
 * the result and wakes its task; a record armed with a linked timeout has
 * a pair of completions, so the pending count falls with every member and
 * the last one frees the record unless the task already left it (then the
 * reaper frees). A cancelled record is marked freed at its cancellation
 * and every later member is skipped here. */
static void uring_reap_cqe(struct pas_uring_cqe *cqe) {
  PasFdWait *rec = (PasFdWait *)(uintptr_t)cqe->user_data;
  if (!rec || rec->freed)
    return;          /* NULL: a poll removal; freed: abandoned or done */
  if (rec->pending > 0)
    rec->pending--;
  if (!rec->seen) {
    rec->seen = 1;
    rec->res = cqe->res;
    rec->freed = rec->pending == 0;
    fd_unlink(rec);
    wake(&rec->w, WAKE_NORMAL);
  } else if (rec->pending == 0) {
    rec->freed = 1;
    if (rec->left)
      free(rec);
  }
}
#endif

/* Called under the lock: submit the kernel interest for rec. On the poll
 * backends the record arms its own dup of fd: an fd may have several
 * records parked at once (see the section comment), and the kernel would
 * otherwise merge or overwrite registrations keyed by the fd alone — epoll
 * drops a second ADD with EEXIST, and a kqueue knote carries one udata, so
 * a second EV_ADD would re-address the knote the first record parked on. */
static void fd_arm(PasFdWait *rec) {
#ifdef __linux__
  if (fd_backend == FD_BACKEND_URING) {
    uring_submit_rec(rec);
    return;
  }
  rec->arm_fd = dup(rec->fd);
  if (rec->arm_fd < 0)
    rec->arm_fd = rec->fd;   /* out of descriptors: share, best effort */
  struct epoll_event ev;
  memset(&ev, 0, sizeof ev);
  ev.events = rec->write ? EPOLLOUT : EPOLLIN;
  ev.data.ptr = rec;
  epoll_ctl(fd_poll_fd, EPOLL_CTL_ADD, rec->arm_fd, &ev);
#else
  rec->arm_fd = dup(rec->fd);
  if (rec->arm_fd < 0)
    rec->arm_fd = rec->fd;   /* out of descriptors: share, best effort */
  struct kevent kn;
  memset(&kn, 0, sizeof kn);
  kn.ident = (uintptr_t)rec->arm_fd;
  kn.filter = rec->write ? EVFILT_WRITE : EVFILT_READ;
  kn.flags = EV_ADD;
  kn.udata = rec;
  kevent(fd_poll_fd, &kn, 1, NULL, 0, NULL);
#endif
  rec->armed = 1;
}

/* Parks the current task on rec (linked); on return the lock is released and
 * rec is disposed of (freed by the task, or abandoned when cancelled on
 * io_uring): *out carries the io_uring completion (recv/send), otherwise
 * the mapped bits (poll backends) or the raw completion (io_uring poll
 * waits). Returns 0 completed, 1 cancelled. */
static int fd_park(PasFdWait *rec, int64_t *out) {
  PasContext *c = context();
  PasTask *me = c->current;
  Observation o;
  observe(me, &o);
  me->state = TS_WAITING;
  me->wake = WAKE_NORMAL;
  UNLOCK();
  suspend(me);
  LOCK();
  me->state = TS_RUNNING;
  unobserve(&o);
  int cancelled = me->wake == WAKE_CANCEL;
#ifdef __linux__
  if (fd_backend == FD_BACKEND_URING) {
    int64_t res = rec->res; /* the completion */
    if (cancelled) {
      /* no free here: a completion may follow the task's rise (or never
       * come) — the record is abandoned, marked freed so completions and
       * submissions skip it */
      rec->state = 1;
      rec->freed = 1;
      fd_waits_abandoned++;
      uring_cancel_rec(rec);   /* best effort: stop a poll still pending */
      fd_unlink(rec);
      UNLOCK();
      *out = res;
      return 1;
    }
    fd_unlink(rec);
    UNLOCK();
    *out = res;
    if (rec->freed)
      free(rec);       /* every member of the pair is reaped: all ours */
    else
      rec->left = 1;   /* the linked timeout may still complete: the
                          reaper that takes pending to 0 frees it */
    return 0;
  }
#endif
  /* poll backends: drop the kernel interest; there is no completion to
   * wait for — the syscall happens in the caller after the wake. The bits
   * the poller mapped in rec->res go to the caller. */
  fd_retire(rec);
  *out = rec->res;
  fd_unlink(rec);
  UNLOCK();
  free(rec);
  return cancelled;
}

#ifdef __linux__
/* Parks the current task on one io_uring op (RECV or SEND): submits it,
 * waits for the completion, and retries the transient results (a readiness
 * race, or a redelivered signal) after re-parking on the fd. The result is
 * the op's completion: what recv/send would return, or negative (-errno);
 * a cancelled wait raises. */
static int64_t fd_uring_op(int op, int32_t fd, void *buf, int32_t len) {
  PasTask *me = context()->current;
  for (;;) {
    int64_t res;
    LOCK();
    if (cancelled_locked(me->token)) {
      UNLOCK();
      raise_cancelled();
    }
    PasFdWait *rec = malloc(sizeof *rec);
    if (!rec)
      __pas_runtime_error("out of memory waiting on a socket");
    memset(rec, 0, sizeof *rec);
    rec->fd = fd;
    rec->arm_fd = -1;
    rec->op = op;
    rec->buf = buf;
    rec->len = len;
    rec->w.next = NULL;
    rec->w.task = me;
    rec->w.thread = NULL;
    rec->w.signaled = 0;
    rec->next = fd_waits;
    fd_waits = rec;
    fd_arm(rec);                    /* submits the op */
    int cancelled = fd_park(rec, &res);
    if (cancelled)
      raise_cancelled();            /* the record was abandoned above */
    if (res >= 0)
      return res;
    if (res == -EINTR)
      continue;
    if (res != -EAGAIN && res != -EWOULDBLOCK)
      return res;                   /* a failure to report to the caller */
    /* lost the readiness race: re-park until readable/writable */
    __pas_task_wait_fd(fd, op == FD_OP_SEND ? 1 : 0);
  }
}
#endif

static void *fd_poller_main(void *arg);

#ifndef __linux__
static void *fd_poller_main(void *arg) {
  (void)arg;
  for (;;) {
    int wait_ms;
    LOCK();
    fd_fire_deadlines();
    wait_ms = fd_poll_wait_ms();
    UNLOCK();
    struct kevent evs[64];
    struct timespec ts;
    ts.tv_sec = wait_ms / 1000;
    ts.tv_nsec = (long)(wait_ms % 1000) * 1000000L;
    int n = kevent(fd_poll_fd, NULL, 0, evs, 64, &ts);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      continue;
    }
    for (int i = 0; i < n; i++) {
      if (evs[i].flags & EV_ERROR)
        continue;      /* disarmed while in flight; nothing to do */
      PasFdWait *rec = evs[i].udata;
      LOCK();
      if (rec->state == 0) {
        rec->res = fd_bits_kqueue(&evs[i]);
        wake(&rec->w, WAKE_NORMAL);
        fd_retire(rec);
      }
      UNLOCK();
    }
  }
  return NULL;
}
#endif

#ifdef __linux__
static void *fd_poller_main(void *arg) {
  (void)arg;
  if (fd_backend == FD_BACKEND_URING) {
    for (;;) {
      /* cheap check without the lock: anything to reap? */
      unsigned head = __atomic_load_n(ur_cq_head, __ATOMIC_ACQUIRE);
      unsigned tail = __atomic_load_n(ur_cq_tail, __ATOMIC_ACQUIRE);
      if (head == tail) {
        /* nothing: block until the next completion (this also submits
         * what pending SQEs tasks left published in the ring) */
        syscall(SYS_io_uring_enter, fd_poll_fd, 0, 1,
                IORING_ENTER_GETEVENTS, NULL, 0);
        continue;
      }
      LOCK();
      /* reap fresh state (tasks may have reaped under the lock meanwhile) */
      head = __atomic_load_n(ur_cq_head, __ATOMIC_RELAXED);
      tail = __atomic_load_n(ur_cq_tail, __ATOMIC_ACQUIRE);
      while (head != tail) {
        uring_reap_cqe(&ur_cqes[head & *ur_sq_mask]);
        head++;
      }
      __atomic_store_n(ur_cq_head, head, __ATOMIC_RELEASE);
      UNLOCK();
    }
  }
  for (;;) {
    int wait_ms;
    LOCK();
    fd_fire_deadlines();
    wait_ms = fd_poll_wait_ms();
    UNLOCK();
    struct epoll_event evs[64];
    int n = epoll_wait(fd_poll_fd, evs, 64, wait_ms);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      continue;
    }
    for (int i = 0; i < n; i++) {
      PasFdWait *rec = evs[i].data.ptr;
      LOCK();
      if (rec->state == 0) {
        rec->res = fd_bits_epoll(evs[i].events);
        wake(&rec->w, WAKE_NORMAL);
        fd_retire(rec);
      }
      UNLOCK();
    }
  }
}
#endif

/* The wait records a cancelled io_uring wait abandoned on purpose (a
 * completion may follow the task's rise, or never come): the record leaks
 * by design, ~72 bytes each, bounded by the number of cancellations. The
 * poll backends retire their records cleanly, so this stays 0 there. Leak
 * tests compare this counter, not the allocator. For unit netasync. */
int64_t __pas_net_waits_abandoned(void) {
  return fd_waits_abandoned;
}

/* The poller backend in place: 0 kevent, 1 epoll, 2 io_uring. For benchmark
 * reports and logs. For unit netasync. */
int32_t __pas_net_fd_backend(void) {
  return fd_backend;
}

/* Marks a descriptor blocking (on 0) or non-blocking, through the C runtime
 * (plain C can call the variadic fcntl; calls emitted by p3 threads cannot).
 * For unit netasync. */
void __pas_fd_nonblock(int32_t fd, int32_t on) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return;
#ifdef __linux__
  if (on)
    flags |= O_NONBLOCK;
  else
    flags &= ~O_NONBLOCK;
#else
  if (on)
    flags |= O_NONBLOCK;
  else
    flags &= ~O_NONBLOCK;
#endif
  fcntl(fd, F_SETFL, flags);
}

/* Called once, from unit netasync's initialization (plain context). */
void __pas_fd_poller_start(void) {
  LOCK();
  fd_poller_start();
  UNLOCK();
}

static void fd_poller_start(void) {
  if (fd_poll_started)
    return;
  fd_poll_started = 1;
#ifdef __linux__
  fd_backend = FD_BACKEND_EPOLL;
  { /* io_uring first; falls back to epoll when the kernel has none */
    struct pas_uring_params params;
    memset(&params, 0, sizeof params);
    int fd = syscall(SYS_io_uring_setup, PAS_URING_ENTRIES, &params);
    if (fd >= 0 && params.sq_off.array && params.cq_off.cqes) {
      size_t sq_len = params.sq_off.array +
                      (size_t)params.sq_entries * sizeof(unsigned);
      size_t cq_len = params.cq_off.cqes +
                      (size_t)params.cq_entries * sizeof(struct pas_uring_cqe);
      size_t sqe_len = (size_t)params.sq_entries * sizeof(struct pas_uring_sqe);
      ur_sq_ring = mmap(NULL, sq_len, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
      ur_cq_ring = mmap(NULL, cq_len, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
      ur_sqes = mmap(NULL, sqe_len, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
      if (ur_sq_ring != MAP_FAILED && ur_cq_ring != MAP_FAILED &&
          ur_sqes != MAP_FAILED) {
        ur_sq_head = (unsigned *)((char *)ur_sq_ring + params.sq_off.head);
        ur_sq_tail = (unsigned *)((char *)ur_sq_ring + params.sq_off.tail);
        ur_sq_mask = (unsigned *)((char *)ur_sq_ring + params.sq_off.ring_mask);
        ur_sq_array = (unsigned *)((char *)ur_sq_ring + params.sq_off.array);
        ur_cq_head = (unsigned *)((char *)ur_cq_ring + params.cq_off.head);
        ur_cq_tail = (unsigned *)((char *)ur_cq_ring + params.cq_off.tail);
        ur_cq_mask = (unsigned *)((char *)ur_cq_ring + params.cq_off.ring_mask);
        ur_cqes = (struct pas_uring_cqe *)((char *)ur_cq_ring +
                                           params.cq_off.cqes);
        /* the SQ array is userspace's job: map each index to itself (the
         * kernel reads SQEs through it — an all-zero array resubmits the
         * first SQE over and over and drops the rest) */
        for (unsigned i = 0; i < params.sq_entries; i++)
          ur_sq_array[i] = i;
        *ur_sq_tail = 0;
        fd_poll_fd = fd;
        fd_backend = FD_BACKEND_URING;
      } else {
        if (ur_sq_ring != MAP_FAILED)
          munmap(ur_sq_ring, sq_len);
        if (ur_cq_ring != MAP_FAILED)
          munmap(ur_cq_ring, cq_len);
        if (ur_sqes != MAP_FAILED)
          munmap(ur_sqes, sqe_len);
        close(fd);
      }
    }
  }
  if (fd_backend != FD_BACKEND_URING) {
    fd_poll_fd = epoll_create1(0);
    if (fd_poll_fd < 0)
      __pas_runtime_error("the netasync poller could not start");
  }
#else
  fd_backend = FD_BACKEND_KQUEUE;
  fd_poll_fd = kqueue();
  if (fd_poll_fd < 0)
    __pas_runtime_error("the netasync poller could not start");
#endif
  pthread_attr_t a2;
  pthread_attr_init(&a2);
  pthread_attr_setdetachstate(&a2, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&fd_poller_thread, &a2, fd_poller_main, NULL) != 0)
    __pas_runtime_error("the netasync poller thread could not start");
  pthread_attr_destroy(&a2);
}

/* Parks the current task until fd is readable (want_write 0) or writable
 * (want_write 1), and reports what fired: its integer result is the bitmask
 * 1 = readable, 2 = writable, 4 = the descriptor is in an error state
 * (POLLERR/POLLHUP revents on the poll backends, a negative POLL_ADD
 * completion on io_uring), and 8 = the deadline expired first (only
 * __pas_task_wait_fd2). Outside a task, it blocks this thread with
 * poll(). A cancelled token woke the task with EOperationCancelled
 * (raised here; on io_uring the record is abandoned and leaks, unlike a
 * deadline, whose pair completes and frees normally). */
int32_t __pas_task_wait_fd(int32_t fd, int32_t want_write) {
  return __pas_task_wait_fd2(fd, want_write, 0);
}

int32_t __pas_task_wait_fd2(int32_t fd, int32_t want_write, int32_t deadline_ms) {
  PasContext *c = context();
  PasTask *me = c->current;
  if (!me) {
    struct pollfd pf;
    memset(&pf, 0, sizeof pf);
    pf.fd = fd;
    pf.events = want_write ? POLLOUT : POLLIN;
    int64_t remain = deadline_ms;
    for (;;) {
      if (deadline_ms > 0 && remain <= 0)
        return 8;
      int poll_ms = 1000;
      if (deadline_ms > 0 && remain < 1000)
        poll_ms = (int)remain;
      int r = poll(&pf, 1, poll_ms);
      if (r > 0) {
        int32_t bits = 0;
        if (pf.revents & POLLIN)
          bits |= 1;
        if (pf.revents & POLLOUT)
          bits |= 2;
        if (pf.revents & (POLLERR | POLLHUP | POLLNVAL))
          bits |= 4;
        return bits;
      }
      if (deadline_ms > 0)
        remain -= poll_ms;
    }
  }
  LOCK();
  if (cancelled_locked(me->token)) {
    UNLOCK();
    raise_cancelled();
  }
  fd_poller_start();
  PasFdWait *rec = malloc(sizeof *rec);
  if (!rec)
    __pas_runtime_error("out of memory waiting on a socket");
  memset(rec, 0, sizeof *rec);
  rec->fd = fd;
  rec->arm_fd = -1;
  rec->write = want_write ? 1 : 0;
  rec->op = want_write ? FD_OP_POLLWRITE : FD_OP_POLLREAD;
  rec->pending = 1;   /* io_uring: one completion; a deadline makes it two */
  if (deadline_ms > 0) {
    rec->has_deadline = 1;
    rec->deadline_ms = deadline_ms;
    rec->deadline_us = fd_now_us() + (int64_t)deadline_ms * 1000;
  }
  rec->w.next = NULL;
  rec->w.task = me;
  rec->w.thread = NULL;
  rec->w.signaled = 0;
  rec->next = fd_waits;
  fd_waits = rec;
#ifdef __linux__
  if (fd_backend == FD_BACKEND_URING && rec->has_deadline)
    rec->pending = 2;
  else
#endif
  if (rec->has_deadline) {
    rec->dnext = fd_deadlines;
    fd_deadlines = rec;
  }
  fd_arm(rec);
  int64_t fired;
  int cancelled = fd_park(rec, &fired);
  if (cancelled)
    raise_cancelled();
#ifdef __linux__
  if (fd_backend == FD_BACKEND_URING) {
    /* raw POLL_ADD revents come in the completion (negative on failure);
     * a deadline's pair carries -ECANCELLED / -ETIME, both a timeout */
    int32_t bits = 0;
    if (fired < 0) {
      if (-fired == ECANCELLED || -fired == ETIME)
        bits = 8;
      else
        bits = 4;
    } else {
      if (fired & 1)
        bits |= 1;               /* POLLIN */
      if (fired & 4)
        bits |= 2;               /* POLLOUT */
      if (fired & (8 | 16 | 32))
        bits |= 4;               /* POLLERR | POLLHUP | POLLNVAL */
    }
    return bits;
  }
#endif
  return (int32_t)fired;   /* the poll backends deliver the mapped bits */
}

/* Parks the current task until recv(fd, buf, len) completes; the result: the
 * count read, 0 at the end of the connection, or negative (-errno). Outside
 * a task: polls, then recvs. */
int64_t __pas_task_recv(int32_t fd, void *buf, int32_t len) {
  PasContext *c = context();
  PasTask *me = c->current;
  if (!me) {
    struct pollfd pf;
    memset(&pf, 0, sizeof pf);
    pf.fd = fd;
    pf.events = POLLIN;
    for (;;) {
      while (poll(&pf, 1, 1000) <= 0)
        ;
      ssize_t n = recv(fd, buf, len, 0);
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        continue;
      return n < 0 ? -(int64_t)errno : n;
    }
  }
#ifdef __linux__
  if (fd_backend == FD_BACKEND_URING) {
    /* native async recv: the completion is the recv result */
    return fd_uring_op(FD_OP_RECV, fd, buf, len);
  }
#endif
  /* poll backends: park until readiness (raises on cancellation), then
   * the recv syscall happens here */
  for (;;) {
    __pas_task_wait_fd(fd, 0);
    ssize_t n = recv(fd, buf, len, 0);
    if (n > 0)
      return n;
    if (n == 0)
      return 0;
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      continue;                     /* lost the readiness race: re-park */
    return -(int64_t)errno;
  }
}

/* Same parking protocol for send(fd, buf, len). */
int64_t __pas_task_send(int32_t fd, void *buf, int32_t len) {
  PasContext *c = context();
  PasTask *me = c->current;
  if (!me) {
    ssize_t n;
    for (;;) {
      n = send(fd, buf, len, MSG_NOSIGNAL);
      if (n >= 0)
        return n;
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        struct pollfd pf;
        memset(&pf, 0, sizeof pf);
        pf.fd = fd;
        pf.events = POLLOUT;
        poll(&pf, 1, 1000);
        continue;
      }
      return -(int64_t)errno;
    }
  }
#ifdef __linux__
  if (fd_backend == FD_BACKEND_URING) {
    /* native async send: the completion is the send result */
    return fd_uring_op(FD_OP_SEND, fd, buf, len);
  }
#endif
  for (;;) {
    ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
    if (n >= 0)
      return n;
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      __pas_task_wait_fd(fd, 0 + 1);
      continue;
    }
    return -(int64_t)errno;
  }
}


/*===- Mutexes and wait queues (for unit threading) -------------------------===*/

typedef struct PasWaitQueue {
  PasWaiter *waiters;
} PasWaitQueue;

void *__pas_mutex_new(void) {
  pthread_mutex_t *m = malloc(sizeof *m);
  if (!m)
    __pas_runtime_error("out of memory for a mutex");
  pthread_mutex_init(m, NULL);
  return m;
}

void __pas_mutex_free(void *m) {
  if (m) {
    pthread_mutex_destroy(m);
    free(m);
  }
}

void __pas_mutex_lock(void *m) { pthread_mutex_lock(m); }
void __pas_mutex_unlock(void *m) { pthread_mutex_unlock(m); }

void *__pas_waitq_new(void) {
  PasWaitQueue *q = calloc(1, sizeof *q);
  if (!q)
    __pas_runtime_error("out of memory for a wait queue");
  return q;
}

void __pas_waitq_free(void *q) { free(q); }

static int woken(void *arg) { return ((PasWaiter *)arg)->signaled; }

/* Like a condition variable: releases mutex m, waits on q until woken, then
 * takes m again. A task waits (cancellable: then EOperationCancelled, with
 * m taken); a thread outside any task runs its tasks meanwhile. */
void __pas_waitq_wait(void *qp, void *m) {
  PasWaitQueue *q = qp;
  PasContext *c = context();
  PasTask *me = c->current;
  LOCK();
  if (!me) {
    PasWaiter w = {NULL, NULL, c, 0};
    add_waiter(&q->waiters, &w);
    pthread_mutex_unlock(m);
    loop_until(c, woken, &w, 0);
    remove_waiter(&q->waiters, &w);
    UNLOCK();
    pthread_mutex_lock(m);
    return;
  }
  if (cancelled_locked(me->token)) {
    UNLOCK();
    raise_cancelled();
  }
  PasWaiter w = {NULL, me, NULL, 0};
  add_waiter(&q->waiters, &w);
  Observation o;
  observe(me, &o);
  me->state = TS_WAITING;
  me->wake = WAKE_NORMAL;
  pthread_mutex_unlock(m);
  UNLOCK();
  suspend(me);
  LOCK();
  me->state = TS_RUNNING;
  unobserve(&o);
  remove_waiter(&q->waiters, &w);
  int cancelled = me->wake == WAKE_CANCEL;
  UNLOCK();
  pthread_mutex_lock(m);
  if (cancelled)
    raise_cancelled();
}

void __pas_waitq_signal(void *qp) {
  PasWaitQueue *q = qp;
  LOCK();
  for (PasWaiter *w = q->waiters; w; w = w->next) {
    if (!w->signaled && (!w->task || w->task->state == TS_WAITING)) {
      wake(w, WAKE_NORMAL);
      break;
    }
  }
  UNLOCK();
}

void __pas_waitq_broadcast(void *qp) {
  PasWaitQueue *q = qp;
  LOCK();
  for (PasWaiter *w = q->waiters; w; w = w->next)
    wake(w, WAKE_NORMAL);
  UNLOCK();
}

/*===- WhenAll / WhenAny -----------------------------------------------------===*/

/* WhenAll's task: awaits each of n tasks, then its result is a dynamic
 * array of their results (elem: their type). */
typedef struct {
  PasTask **tasks;
  int64_t n;
  const PasDynInfo *elem;
} WhenArgs;

static void when_all_entry(PasTask *self) {
  WhenArgs *a = __pas_task_args(self);
  for (int64_t i = 0; i < a->n; ++i)
    __pas_task_await(a->tasks[i]);
  void **result = __pas_task_result(self);
  if (a->elem) {
    char *arr = __pas_dyn_new(a->elem, a->n);
    for (int64_t i = 0; i < a->n; ++i) {
      memcpy(arr + i * a->elem->elem_size, __pas_task_result(a->tasks[i]),
             (size_t)a->elem->elem_size);
      if (a->elem->addref)
        a->elem->addref(arr + i * a->elem->elem_size);
    }
    *result = arr;
  }
}

static void when_any_entry(PasTask *self) {
  WhenArgs *a = __pas_task_args(self);
  int32_t *result = __pas_task_result(self);
  PasContext *c = context();
  PasTask *me = c->current;
  LOCK();
  for (;;) {
    for (int64_t i = 0; i < a->n; ++i)
      if (task_ended(a->tasks[i])) {
        UNLOCK();
        *result = (int32_t)i;
        return;
      }
    if (cancelled_locked(me->token)) {
      UNLOCK();
      raise_cancelled();
    }
    PasWaiter ws[64];
    int64_t k = a->n < 64 ? a->n : 64;
    for (int64_t i = 0; i < k; ++i) {
      memset(&ws[i], 0, sizeof ws[i]);
      ws[i].task = me;
      add_waiter(&a->tasks[i]->waiters, &ws[i]);
    }
    Observation o;
    observe(me, &o);
    me->state = TS_WAITING;
    me->wake = WAKE_NORMAL;
    UNLOCK();
    suspend(me);
    LOCK();
    me->state = TS_RUNNING;
    unobserve(&o);
    for (int64_t i = 0; i < k; ++i)
      remove_waiter(&a->tasks[i]->waiters, &ws[i]);
    if (me->wake == WAKE_CANCEL) {
      UNLOCK();
      raise_cancelled();
    }
  }
}

static void when_fini(void *args) {
  WhenArgs *a = args;
  for (int64_t i = 0; i < a->n; ++i)
    __pas_task_release(a->tasks[i]);
  free(a->tasks);
}

static PasTask *when_new(PasTask *const *tasks, int64_t n, const PasDynInfo *result,
                         void (*entry)(PasTask *), const PasDynInfo *elem) {
  PasTask *t = __pas_task_new(result, sizeof(WhenArgs), entry, when_fini);
  WhenArgs *a = __pas_task_args(t);
  a->n = n;
  a->elem = elem;
  a->tasks = malloc((size_t)(n ? n : 1) * sizeof(PasTask *));
  for (int64_t i = 0; i < n; ++i) {
    if (!tasks[i])
      __pas_runtime_error("WhenAll/WhenAny of a nil task");
    a->tasks[i] = tasks[i];
    __pas_task_addref(tasks[i]);
  }
  __pas_task_start(t);
  return t;
}

/* WhenAll(tasks): result_info is that of the dynamic array, elem that of
 * the tasks' results (NULL: no results, no array). */
PasTask *__pas_when_all(PasTask *const *tasks, int64_t n, const PasDynInfo *result_info,
                        const PasDynInfo *elem) {
  return when_new(tasks, n, result_info, when_all_entry, elem);
}

PasTask *__pas_when_any(PasTask *const *tasks, int64_t n, const PasDynInfo *int_info) {
  return when_new(tasks, n, int_info, when_any_entry, NULL);
}

/*===- Iterators (generators) ------------------------------------------------===*/
/* An iterator routine's call is a generator: its body runs on a stack of
 * its own, from the first __pas_gen_next until it yields (a value in its
 * slot) or ends; each __pas_gen_next resumes it. No scheduler: the
 * consumer and the generator switch directly, on the consumer's thread.
 * Dropping the last reference to a suspended generator resumes it once
 * more with `stopping` set: its yield raises EIteratorStop (a TObject, not
 * an Exception), so its finally parts run, and the stop ends it. */

enum { GS_NEW, GS_RUNNING, GS_SUSPENDED, GS_DONE };

typedef struct PasGen {
  int64_t refs;
  int state;
  int stopping;
  int has_value;                  /* the slot holds a value (to release) */
  const PasDynInfo *info;         /* the values' type */
  void (*entry)(struct PasGen *);
  void (*args_fini)(void *args);
  int64_t value_size;
  ucontext_t ctx, back;           /* its own; the consumer's, while it runs */
  void *stack;
  const void *back_bottom;        /* (the consumer's stack, for ASan) */
  size_t back_size;
  void *fake_stack;
  PasExcFrame *exc_top;           /* its exception frames, while switched out */
  void *exception;                /* it failed: the exception, until raised */
  const char *exc_where;
  struct PasGen *outer;           /* the generator running before it */
  char data[];                    /* the value, then the arguments */
} PasGen;

/* The generator whose body is running on this thread (yield's). */
_Thread_local PasGen *__pas_gen_current;

extern void *const __pas_iteratorstop_vmt[7];

PasGen *__pas_gen_new(const PasDynInfo *info, int64_t args_size,
                      void (*entry)(PasGen *), void (*args_fini)(void *)) {
  int64_t vs = round16(info->elem_size);
  PasGen *g = calloc(1, sizeof(PasGen) + (size_t)vs + (size_t)args_size);
  if (!g)
    __pas_runtime_error("out of memory for an iterator");
  g->refs = 1;
  g->info = info;
  g->entry = entry;
  g->args_fini = args_fini;
  g->value_size = vs;
  return g;
}

void *__pas_gen_args(PasGen *g) { return g->data + g->value_size; }
void *__pas_gen_value(PasGen *g) { return g->data; }

static void gen_main(void) {
  PasGen *g = __pas_gen_current;
#ifdef PAS_ASAN
  __sanitizer_finish_switch_fiber(NULL, &g->back_bottom, &g->back_size);
#endif
  PasExcFrame f;
  memset(&f, 0, sizeof f);
  __pas_exc_push(&f);
  if (_setjmp(*(jmp_buf *)(void *)f.buf) == 0) {
    g->entry(g);
    __pas_exc_pop(&f);
  } else if (f.object && *(void *const **)f.object == __pas_iteratorstop_vmt) {
    __pas_free_object(f.object); /* stopped: an ordinary end */
  } else {
    g->exception = f.object;
    g->exc_where = f.where;
  }
  if (g->args_fini)
    g->args_fini(__pas_gen_args(g));
  g->args_fini = NULL;
  g->state = GS_DONE;
  g->exc_top = NULL;
#ifdef PAS_ASAN
  __sanitizer_start_switch_fiber(NULL, g->back_bottom, g->back_size);
#endif
  setcontext(&g->back); /* (its stack is freed there) */
  __pas_runtime_error("an iterator resumed after it ended");
}

/* Runs g until it yields or ends. */
static void gen_resume(PasGen *g) {
  if (g->state == GS_NEW) {
    g->stack = new_stack();
    getcontext(&g->ctx);
    g->ctx.uc_stack.ss_sp = g->stack;
    g->ctx.uc_stack.ss_size = STACK_SIZE;
    g->ctx.uc_link = NULL;
    makecontext(&g->ctx, gen_main, 0);
  }
  PasExcFrame *saved = __pas_exc_top;
  __pas_exc_top = g->exc_top;
  g->outer = __pas_gen_current;
  __pas_gen_current = g;
  g->state = GS_RUNNING;
#ifdef PAS_ASAN
  void *fake = NULL;
  __sanitizer_start_switch_fiber(&fake, g->stack, STACK_SIZE);
#endif
  swapcontext(&g->back, &g->ctx);
#ifdef PAS_ASAN
  __sanitizer_finish_switch_fiber(fake, NULL, NULL);
#endif
  __pas_gen_current = g->outer;
  __pas_exc_top = saved;
  if (g->state == GS_DONE && g->stack) {
    free_stack(g->stack);
    g->stack = NULL;
  }
}

/* The next value: 1 (in the slot), or 0 at the end; raises what the body
 * raised. */
int32_t __pas_gen_next(PasGen *g) {
  if (!g)
    __pas_runtime_error("use of a nil iterator");
  if (g->state == GS_RUNNING)
    __pas_runtime_error("an iterator asked for its next value while it runs");
  if (g->state == GS_DONE)
    return 0;
  gen_resume(g);
  if (g->exception) {
    void *e = g->exception;
    g->exception = NULL;
    __pas_raise(e, g->exc_where ? g->exc_where : "");
  }
  return g->state == GS_SUSPENDED;
}

/* In an iterator's body, before a value is stored: its slot. */
void *__pas_gen_slot(void) {
  PasGen *g = __pas_gen_current;
  if (!g)
    __pas_runtime_error("yield outside an iterator");
  g->has_value = 1;
  return g->data;
}

static void raise_stop(void) __attribute__((noreturn));
static void raise_stop(void) {
  __pas_raise_message(__pas_iteratorstop_vmt, "iterator stopped");
}

/* In an iterator's body, its value stored: back to the consumer until the
 * next value is asked for. */
void __pas_gen_yield(void) {
  PasGen *g = __pas_gen_current;
  if (!g)
    __pas_runtime_error("yield outside an iterator");
  if (g->stopping)
    raise_stop();
  g->state = GS_SUSPENDED;
  g->exc_top = __pas_exc_top;
#ifdef PAS_ASAN
  __sanitizer_start_switch_fiber(&g->fake_stack, g->back_bottom, g->back_size);
#endif
  swapcontext(&g->ctx, &g->back);
#ifdef PAS_ASAN
  __sanitizer_finish_switch_fiber(g->fake_stack, &g->back_bottom, &g->back_size);
#endif
  if (g->stopping)
    raise_stop();
}

void __pas_gen_addref(PasGen *g) {
  if (g)
    __atomic_fetch_add(&g->refs, 1, __ATOMIC_RELAXED);
}

void __pas_gen_release(PasGen *g) {
  if (!g || __atomic_sub_fetch(&g->refs, 1, __ATOMIC_ACQ_REL) != 0)
    return;
  if (g->state == GS_SUSPENDED) { /* its finally parts run */
    g->stopping = 1;
    g->refs = 1; /* (alive while it unwinds) */
    gen_resume(g);
    if (g->exception) { /* (raised while it unwound: dropped) */
      __pas_free_object(g->exception);
      g->exception = NULL;
    }
  }
  if (g->state == GS_NEW && g->args_fini)
    g->args_fini(__pas_gen_args(g));
  if (g->has_value && g->info->fini)
    g->info->fini(g->data);
  free(g);
}

void __pas_gen_assign(PasGen **dst, PasGen *src) {
  __pas_gen_addref(src);
  PasGen *old = *dst;
  *dst = src;
  __pas_gen_release(old);
}

void __pas_gen_clear(PasGen **p) {
  PasGen *old = *p;
  *p = NULL;
  __pas_gen_release(old);
}
