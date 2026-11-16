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
#include <pthread.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

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

extern void *const __pas_eoperationcancelled_vmt[5];
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

/* Under the lock: the pool, started when first needed. */
static PasContext *pool_context(void) {
  if (!__pas_async_pool) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    const char *env = getenv("PCC_POOL_THREADS");
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

/* Cancellation checked by hand (TCancelToken.ThrowIfCancelled). */
void __pas_token_check(PasToken *k) {
  if (__pas_token_cancelled(k))
    raise_cancelled();
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
